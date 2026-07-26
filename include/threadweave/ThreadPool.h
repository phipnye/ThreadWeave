#ifndef TW_THREAD_POOL_H
#define TW_THREAD_POOL_H

#include <threadweave/ChaseLevDeque.h>
#include <threadweave/Future.h>
#include <threadweave/NodeAllocator.h>
#include <threadweave/VyukovQueue.h>
#include <threadweave/utils.h>

#include <atomic>
#include <cassert>
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
  using FutureNodeBase = Internal::FutureNodeBase;

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
  std::unique_ptr<ChaseLevDeque<FutureNodeBase*>[]> workerDeques_;

  // Global MPSC queue for storing tasks submitted by non-workers
  VyukovQueue<FutureNodeBase*> injectionQueue_;

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
   * Execute a task stored in a future base node pointer, set its status to
   * active, and then decrements counters with proper ordering
   * @param task a pointer to a future base node with a task to execute
   */
  void executeTask(FutureNodeBase* task);

  /**
   * Helper to retrieve the current state values
   * @param order Memory ordering to use on the load
   * @return the current number of non-executing queued tasks and the pool
   * stop values
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
   * Helper function for a thread to wait on a future node result to be ready.
   * If the caller is a worker thread in a thread pool, it continues executing
   * work until results are ready. Otherwise, non-workers fallback to atomic
   * parking behavior.
   * @param node a pointer to the futrue node to wait on
   */
  static void awaitNode(FutureNodeBase* node);

 private:
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
  using Node = Internal::FutureNode<ReturnType>;
  using Allocator = Internal::NodeAllocator<Node>;
  static_assert(!std::is_reference_v<ReturnType>,
                "Reference return types are not supported directly. Return a "
                "pointer or std::reference_wrapper instead.");

  // Package the functions and arguments into a lambda
  auto boundTask{
      [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
        return std::invoke(std::move(f), std::move(args)...);
      }};

  // Future node uses an internal buffer to store function payload that is
  // aligned using max_align_t, ensure the passed task doesn't violate size
  // constraints for this buffer
  using BoundTask = decltype(boundTask);
  static_assert(
      sizeof(BoundTask) <= Node::kPayloadSize,
      "Task arguments exceed the FutureNode's internal buffer limit. "
      "Consider reducing the size of passed arguments or define the macro "
      "TW_PAYLOAD_SIZE to increase the size of the internal buffer.");
  static_assert(alignof(BoundTask) <= alignof(std::max_align_t),
                "Task's captured state requires stricter alignment than the "
                "FutureNode payload buffer guarantees.");

  // Retrive an allocation for a node (note deallocation is taken care of by the
  // future's destructor or the lambda where release() is called)
  Node* const taskNode{Allocator::allocate()};

  // Construct the callable object directly inside the node's byte payload
  static_assert(
      std::is_nothrow_move_constructible_v<BoundTask>,
      "BoundTask's move constructor may throw and cause a memory leak");
  ::new (static_cast<void*>(taskNode->payload)) BoundTask{std::move(boundTask)};

  // Bind the execution layout
  taskNode->execute = [](FutureNodeBase* const base) {
    // Re-cast back to a node pointer
    Node* const tskNode{static_cast<Node*>(base)};

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
    // Payload is of type std::byte[] and decays to a byte* thus launder is
    // necessary here to prevent violating 2.
    auto* const bndTask{
        std::launder(reinterpret_cast<BoundTask*>(tskNode->payload))};

    if constexpr (std::is_void_v<ReturnType>) {
      try {
        (*bndTask)();
      } catch (...) {
        tskNode->exception = std::current_exception();
      }
    } else {
      try {
        ::new (static_cast<void*>(tskNode->resultBuffer))
            ReturnType{(*bndTask)()};
        tskNode->hasResult = true;  // helps track need to call destructor
      } catch (...) {
        tskNode->exception = std::current_exception();
      }
    }

    // Explicitly clean up the bound lambda and arguments and then notify thread
    // of completion
    bndTask->~BoundTask();
    tskNode->notify();

    // Decrement node's internal refernce count and deallocate node if caller is
    // last one holding a reference
    if (tskNode->release()) {
      Allocator::deallocate(tskNode);
    }
  };

  // Increment both task counters (note sequential consistency synchronizes with
  // worker loop and prevents store-load reordering that could cause a
  // lost wakeup and indefinite parking
  // Submit:       1) Marks task queued   -> 2) Checks if worker asleep
  // WorkerLoop(): 1) Marks worker parked -> 2) Checks if task is queued
  // With weaker memory orderings, both threads could see old memory at
  // step 2 and if both read 0, we get deadlock
  nPendingTasks_.fetch_add(1, MemoryOrder::relaxed);
  incrementNumQueued(MemoryOrder::seq_cst);

  // Wake up parked workers waiting on queued work
  if (nParkedWorkers_.load(MemoryOrder::seq_cst) > 0) {
    state_.notify_one();
  }

  if (currentPool == this) {
    // If thread submitting task is a worker, push the task directly to its own
    // work deque
    assert(workerId >= 0 && workerId < nThreads_ && "Out of bounds threadId");
    workerDeques_[workerId].push(taskNode);
  } else {
    // Otherwise, push the task to the global injection queue that a worker will
    // later take and store in its own Chase Lev Deque
    injectionQueue_.push(taskNode);
  }

  return Future<ReturnType>{taskNode};
}

}  // namespace ThreadWeave

#endif
