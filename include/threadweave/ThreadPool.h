#ifndef TW_THREAD_POOL_H
#define TW_THREAD_POOL_H

#include <threadweave/ChaseLevDeque.h>
#include <threadweave/Future.h>
#include <threadweave/NodeAllocator.h>
#include <threadweave/VyukovQueue.h>
#include <threadweave/internal/utils.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ThreadWeave {

/**
 * An implementation of a thread pool class built upon Vyukov MPSC queues of
 * tasks for non-worker threads to submit to and thread-specific Chase-Lev
 * Deques with the capability for threads to steal tasks from one another.
 */
class ThreadPool {
  // Thread-local information that keeps track of whether the thread submitting
  // a task is a worker in which case it can write directly to its own work
  // deque that must follow SPMC semantics
  static thread_local inline ThreadPool* currentPool{nullptr};
  static thread_local inline Index workerId{-1};

  // Bit tools state manipulation
  static constexpr std::uint64_t kStopMask{1ULL};
  static constexpr std::uint64_t kTaskShift{1ULL};
  static constexpr std::uint64_t kTaskUnit{1ULL << kTaskShift};

  // Maximum number of tasks a worker can drain from the injection queue at a
  // time
  static constexpr Index kMaxDrain{16};

  // --- Data members

  // Worker-specific deques that other workers can steal from
  std::unique_ptr<ChaseLevDeque<Internal::TaskBase*>[]> workerDeques_;

  // Global MPSC queue for storing tasks submitted by non-workers
  VyukovQueue<Internal::TaskBase*> injectionQueue_;

  // Threads
  std::vector<std::thread> workers_;
  const Index nThreads_;

  // State of the pool (LSB stores signal to stop and remaining bits store the
  // number of queued and non-executing tasks the queue still has)
  alignas(Internal::kCacheLineSize) std::atomic<std::uint64_t> state_;

  // Number of tasks (in queue OR executing) the pool still has
  alignas(Internal::kCacheLineSize) std::atomic<Index> nPendingTasks_;

  // Number of workers that are parked
  alignas(Internal::kCacheLineSize) std::atomic<Index> nParkedWorkers_;

  // Mutual exclusion key for preventing multiple consumers of injection queue
  alignas(Internal::kCacheLineSize) std::atomic_flag injectionKey_;

 public:
  // --- Ctors, Assignment, and Dtor

  /**
   * Spawn a thread pool with the specified number of threads. Defaults to
   * std::thread::hardware_concurrency() if unspecified.
   * @param nThreads a positive number of workers to spawn
   */
  explicit ThreadPool(Index nThreads = std::thread::hardware_concurrency());

  // Prevent copy and move ops
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  /**
   * Finish all remaining tasks submitted to the thread pool and join the
   * workers
   */
  ~ThreadPool();

  // --- Member functions

  /**
   * Submit a task to the thread pool
   * @tparam F A generic type of function
   * @tparam Args A generic type of arguments passed to F
   * @param f Function to call with args
   * @param args Arguments passed to f
   * @return A future instance with the result returned by f(args) or an
   * exception if one was thrown
   */
  template <class F, class... Args>
  auto submit(F&& f, Args&&... args)
      -> Future<std::invoke_result_t<F, Args...>>;

 private:
  /**
   * Loop for threads to execute while the thread pool is active
   * @param threadId the unique identifier/index of the thread indicating the
   * index of its resources like its Chase-Lev deque
   */
  void workerLoop(Index threadId);

  /**
   * Try to have the passed thread execute a task if any tasks are available
   * @param threadId the unique identifier/index of the thread to try to have
   * execute a task
   * @return true if a task was executed, false otherwise
   */
  bool tryExecuteTask(Index threadId);

  /**
   * Execute a task, set its status to active, and then decrement counters with
   * proper ordering
   * @param task a pointer to a task to execute
   */
  void executeTask(Internal::TaskBase* task);

  /**
   * Helper to retrieve the current state values
   * @param order Memory ordering to use on the load
   * @return the current state, number of non-executing queued tasks, and the
   * pool stop values
   */
  std::tuple<std::uint64_t, std::uint64_t, bool> getState(
      std::memory_order order) const noexcept;

  /**
   * Helper to set the stop pool state value
   * @param order Memory ordering to use on the store
   */
  void setStop(std::memory_order order) noexcept;

  /**
   * Helper to increment the number of queued tasks
   * @param order Memory ordering to use on the store
   */
  void incrementNumQueued(std::memory_order order) noexcept;

  /**
   * Helper to decrement the number of queued tasks
   * @param order Memory ordering to use on the store
   */
  void decrementNumQueued(std::memory_order order) noexcept;

 public:
  /**
   * Helper function for a thread to wait on a task result to be ready. If the
   * caller is a worker thread in a thread pool, it continues executing work
   * until results are ready. Otherwise, non-workers fallback to atomic parking
   * behavior.
   * @param task a pointer to a task to wait on
   */
  static void awaitTask(Internal::TaskBase* task);

 private:
  /**
   * Helper entry point for tasks submitted to the pool
   * @tparam ReturnType type of the value returned from the submitted task
   * @tparam Callable a bound task callable
   * @param base a pointer to a task base object for the submitted task
   */
  template <typename ReturnType, typename Callable>
  static void taskEntryPoint(Internal::TaskBase* base);

  /**
   * Helper RAII guard for acquiring and releasing injection key
   */
  class KeyGuard {
    // Data members
    std::atomic_flag& key_;
    bool keyAcquired_;

   public:
    // Ctor tries to acquire key
    explicit KeyGuard(std::atomic_flag& key);

    // Dtor releases key if acquired
    ~KeyGuard();

    // Determine if the guard holds the key
    bool holdsKey() const noexcept;
  };
};

template <typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> Future<std::invoke_result_t<F, Args...>> {
  using ReturnType = std::invoke_result_t<F, Args...>;
  using TaskType = Internal::Task<ReturnType>;
  using Allocator = Internal::NodeAllocator<TaskType>;
  static_assert(!std::is_reference_v<ReturnType>,
                "Reference return types are not supported directly. Return a "
                "pointer or std::reference_wrapper instead.");

  // Package the functions and arguments into a lambda
  auto callable{
      [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
        return std::invoke(std::move(f), std::move(args)...);
      }};

  // Task uses an internal buffer to store function payload that is aligned
  // using max_align_t, ensure the passed task doesn't violate size constraints
  // for this buffer
  using Callable = decltype(callable);
  static_assert(
      sizeof(Callable) <= TaskType::kCallableStorageSize,
      "Task arguments exceed the Tasks's internal buffer limit. Consider "
      "reducing the size of passed arguments or define the macro "
      "TW_CALLABLE_STORAGE_SIZE to increase the size of the internal buffer.");
  static_assert(alignof(Callable) <= alignof(std::max_align_t),
                "Task's captured state requires stricter alignment than the "
                "Task callable storage buffer guarantees.");

  // Retrive an allocation for a task (note deallocation is taken care of by the
  // future's destructor or the lambda where releaseReference() is called)
  TaskType* const task{Allocator::allocate()};

  // Construct the callable object directly inside the task's callable storage
  static_assert(
      std::is_nothrow_move_constructible_v<Callable>,
      "BoundTask's move constructor may throw and cause a memory leak");
  ::new (static_cast<void*>(task->callableStorage_))
      Callable{std::move(callable)};

  // Bind the execution layout
  task->execute_ = &ThreadPool::taskEntryPoint<ReturnType, Callable>;

  // Increment both task counters
  nPendingTasks_.fetch_add(1, MemoryOrder::relaxed);
  incrementNumQueued(MemoryOrder::relaxed);

  // Sequential consistency synchronizes with worker loop and prevents
  // store-load reordering that could cause a lost wakeup and indefinite parking
  // Submit:       1) Marks task queued   -> 2) Checks if worker asleep
  // WorkerLoop(): 1) Marks worker parked -> 2) Checks if task is queued
  // With weaker memory orderings, both threads could see old memory at
  // step 2 and if both read 0, we get deadlock
  std::atomic_thread_fence(MemoryOrder::seq_cst);

  // Wake up parked workers waiting on queued work
  if (nParkedWorkers_.load(MemoryOrder::relaxed) > 0) {
    state_.notify_one();
  }

  if (currentPool == this) {
    // If thread submitting task is a worker, push the task directly to its own
    // work deque
    TW_ASSERT(workerId >= 0 && workerId < nThreads_, "Out of bounds threadId");
    workerDeques_[workerId].push(task);
  } else {
    // Otherwise, push the task to the global injection queue that a worker will
    // later take and store in its own Chase Lev Deque
    injectionQueue_.push(task);
  }

  return Future<ReturnType>{task};
}

template <typename ReturnType, typename Callable>
void ThreadPool::taskEntryPoint(Internal::TaskBase* const base) {
  using TaskType = Internal::Task<ReturnType>;
  using Allocator = Internal::NodeAllocator<TaskType>;

  TaskType* const task{static_cast<TaskType*>(base)};
  TW_ASSERT((reinterpret_cast<std::uintptr_t>(task->callableStorage_) %
             alignof(Callable)) == 0,
            "Callable storage buffer misaligned for task target type");

  // Per the standard, a new object is only "transparently replaceable"
  // (meaning you can keep using the old pointer without UB) if all of the
  // following conditions are met:
  // 1. The new object is allocated at the exact same address as the old one.
  // 2. The new object is the exact same type as the old one (ignoring
  // cv-qualifiers).
  // 3. The type does not contain any const-qualified fields (at any level of
  // nesting).
  // 4. The type does not contain any reference fields (at any level of
  // nesting).
  // 5. Both the old and new objects are the most-derived object (i.e., you
  // aren't replacing a base class subobject of a larger class).
  // callableStorage is of type std::byte[] and decays to a byte* thus launder
  // is necessary here to prevent violating 2.
  auto* const callable{
      std::launder(reinterpret_cast<Callable*>(task->callableStorage_))};

  if constexpr (std::is_void_v<ReturnType>) {
    try {
      (*callable)();
    } catch (...) {
      task->exception_ = std::current_exception();
    }
  } else {
    try {
      ::new (static_cast<void*>(task->resultStorage_))
          ReturnType{(*callable)()};
      task->hasResult_ = true;  // helps track need to call destructor
    } catch (...) {
      task->exception_ = std::current_exception();
    }
  }

  // Explicitly clean up the bound lambda and arguments and then notify thread
  // of completion
  callable->~Callable();
  task->notify();

  // Decrement task's internal refernce count and deallocate if caller is
  // last one holding a reference
  if (task->releaseReference()) {
    Allocator::deallocate(task);
  }
}

}  // namespace ThreadWeave

#endif
