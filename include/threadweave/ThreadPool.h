#ifndef TW_THREAD_POOL_H
#define TW_THREAD_POOL_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <random>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "threadweave/ChaseLevDeque.h"
#include "threadweave/VyukovQueue.h"
#include "threadweave/internal/BlockedRange.h"
#include "threadweave/internal/Future.h"
#include "threadweave/internal/NodeAllocator.h"
#include "threadweave/internal/Task.h"
#include "threadweave/internal/sorting.h"
#include "threadweave/internal/utils.h"

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
  template <typename F, typename... Args>
  auto submit(F&& f, Args&&... args)
      -> Future<std::invoke_result_t<F, Args...>>;

  /**
   * Apply a function to each element in a range, splitting work across the
   * thread pool for sufficiently large ranges
   * @tparam Iter A generic iterator type
   * @tparam F A generic type of function, called once per element
   * @param begin Iterator to the start of the range
   * @param end Iterator to the end of the range
   * @param f Function invoked as f(*it) for each element in [begin, end)
   * @param grainSize Minimum range size below which work is done sequentially
   * instead of being split further (defaults to 500)
   */
  template <typename Iter, typename F>
  void forEach(Iter begin, Iter end, F f,
               std::iterator_traits<Iter>::difference_type grainSize = 500);

  /**
   * Reduce a range to a single value by combining elements, splitting work
   * across the thread pool for sufficiently large ranges
   * @tparam T The type of the accumulated result
   * @tparam Iter A generic iterator type
   * @tparam F A generic type of function, used to combine two partial results
   * into one
   * @param begin Iterator to the start of the range
   * @param end Iterator to the end of the range
   * @param init Initial value for the reduction, combined with the result
   * of the range exactly once — does NOT need to be an identity element for f
   * @param f Binary function invoked as f(partial, *it) to fold elements into
   * a partial result, and as f(lhsResult, rhsResult) to combine partial results
   * from split sub-ranges
   * @param grainSize Minimum range size below which work is done sequentially
   * instead of being split further
   * @return init combined with the accumulated result of folding f over [begin,
   * end)
   */
  template <typename T, typename Iter, typename F>
  T reduce(Iter begin, Iter end, T init, F f,
           std::iterator_traits<Iter>::difference_type grainSize = 500);

 private:
  /**
   * Fold a non-empty range into a single value using f
   */
  template <typename T, typename Iter, typename F>
  T reduceRange(Iter begin, Iter end, F f,
                std::iterator_traits<Iter>::difference_type grainSize);

 public:
  /**
   * Sort an object that support random access iterators
   * @tparam Iter a random access iterator type
   * @tparam Compare a comparison functor
   * @param begin the beginning of the range to sort
   * @param end the end of the range to sort
   * @param comp a comparison functor instance (defaults to std::less<>)
   */
  template <typename Iter, typename Compare = std::less<
                               typename std::iterator_traits<Iter>::value_type>>
    requires(std::random_access_iterator<Iter>)
  void sort(Iter begin, Iter end, Compare comp = Compare{});

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
   * Sample two worker deques to try to steal from and return the index of the
   * victim with seemingly more work
   * @return the index of a deque to try to steal from
   */
  Index getVictim(Index threadId) const noexcept;

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

  /**
   * Helper function for a thread to wait on a task result to be ready. If the
   * caller is a worker thread in a thread pool, it continues executing work
   * until results are ready. Otherwise, non-workers fallback to atomic parking
   * behavior.
   * @param task a pointer to a task to wait on
   */
  static void awaitTask(Internal::TaskBase* task);

  // Future<T>::wait() calls await task
  template <typename T, typename WaitPolicy>
  friend class Future;

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

inline ThreadPool::ThreadPool(const Index nThreads)
    : workerDeques_{
          std::make_unique<ChaseLevDeque<Internal::TaskBase*>[]>(nThreads)},
      injectionQueue_{},
      workers_{},
      nThreads_{nThreads},
      state_{0},
      nPendingTasks_{0},
      nParkedWorkers_{0},
      injectionKey_{} {
  // Fill pool with worker threads
  TW_ASSERT(nThreads > 0, "Number of threads must be positive");
  TW_ASSERT(nThreads <= Internal::kMaxThreads,
            "Number of threads exceeds max number of threads. Consider "
            "defining TW_MAX_THREADS with a larger value or decreasing the "
            "number of threads.");
  workers_.reserve(nThreads);

  try {
    for (Index threadId{0}; threadId < nThreads; ++threadId) {
      workers_.emplace_back(&ThreadPool::workerLoop, this, threadId);
    }
  } catch (...) {
    // In case spawning a thread throws, clean up right away
    setStop(MemoryOrder::release);
    state_.notify_all();

    for (auto& t : workers_) {
      if (t.joinable()) {
        t.join();
      }
    }

    throw;
  }
}

inline ThreadPool::~ThreadPool() {
  // Indicate to the threads to stop
  setStop(MemoryOrder::release);
  state_.notify_all();

  // Join all of the workers
  for (auto& t : workers_) {
    t.join();
  }

  TW_DEBUG_ONLY(
      const auto [_state, _nQueued, _stop]{getState(MemoryOrder::relaxed)};
      TW_ASSERT(_nQueued == 0,
                "Thread pool destroyed with unexecuted queued tasks remaining");
      TW_ASSERT(nPendingTasks_.load(MemoryOrder::relaxed) == 0,
                "Thread pool destroyed with pending tasks remaining"););
}

template <typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> Future<std::invoke_result_t<F, Args...>> {
  using ReturnType = std::invoke_result_t<F, Args...>;
  using TaskType = Internal::Task<ReturnType>;
  using Allocator = Internal::NodeAllocator<TaskType>;
  static_assert(!std::is_reference_v<ReturnType>,
                "Reference return types are not supported directly. Return a "
                "pointer or std::reference_wrapper instead.");
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

template <typename Iter, typename F>
void ThreadPool::forEach(
    Iter begin, Iter end, F f,
    const typename std::iterator_traits<Iter>::difference_type grainSize) {
  Internal::BlockedRange<Iter> blockRng{begin, end, grainSize};

  // When insufficient range left, perform tasks sequentially
  if (!blockRng.isDivisible()) {
    for (Iter it{begin}; it != end; ++it) {
      f(*it);
    }

    return;
  }

  // Submit RHS to pool and have current worker continue working on LHS
  Internal::BlockedRange<Iter> rhs{blockRng.split()};
  auto futR{submit(
      [=, this] { this->forEach(rhs.begin(), rhs.end(), f, grainSize); })};
  forEach(blockRng.begin(), blockRng.end(), f, grainSize);
  futR.wait();
}

template <typename T, typename Iter, typename F>
T ThreadPool::reduce(
    Iter begin, Iter end, T init, F f,
    const typename std::iterator_traits<Iter>::difference_type grainSize) {
  static_assert(std::is_same_v<std::invoke_result_t<F, T, decltype(*begin)>, T>,
                "Function f does not return type T when folding an element");
  static_assert(
      std::is_same_v<std::invoke_result_t<F, T, T>, T>,
      "Function f does not return type T when combining two partial results");

  if (begin == end) [[unlikely]] {
    return init;
  }

  return f(std::move(init), reduceRange<T, Iter, F>(begin, end, f, grainSize));
}

template <typename T, typename Iter, typename F>
T ThreadPool::reduceRange(
    Iter begin, Iter end, F f,
    const typename std::iterator_traits<Iter>::difference_type grainSize) {
  TW_ASSERT(grainSize > 0, "Expected a positive grain size");
  TW_ASSERT(begin != end, "Expected a non-empty range");
  Internal::BlockedRange<Iter> blockRng{begin, end, grainSize};

  if (!blockRng.isDivisible()) {
    Iter it{begin};
    T res{*it};

    for (++it; it != end; ++it) {
      res = f(res, *it);
    }

    return res;
  }

  // Split the full current range into a LHS and RHS - note that split should
  // never return an empty range for either side
  Internal::BlockedRange<Iter> rhsRng{blockRng.split()};
  auto futR{submit([=, this] {
    return this->reduceRange<T, Iter, F>(rhsRng.begin(), rhsRng.end(), f,
                                         grainSize);
  })};
  const T lhs{
      reduceRange<T, Iter, F>(blockRng.begin(), blockRng.end(), f, grainSize)};
  return f(lhs, futR.get());
}

template <typename Iter, typename Compare>
  requires(std::random_access_iterator<Iter>)
void ThreadPool::sort(Iter begin, Iter end, Compare comp) {
  // TODO: Verify for std::reference_wrapper
  using DistType = std::iterator_traits<Iter>::difference_type;
  constexpr DistType kCutoff{500};
  const DistType dist{std::distance(begin, end)};

  // Fewer than cutoff elements, fallback to std::sort
  if (dist < kCutoff) {
    std::sort(begin, end, comp);
    return;
  }

  Iter piv{Internal::pickPivot(begin, end, comp)};

  // Only submit LHS if sufficient number of elements
  if (Iter mid{Internal::partition(begin, end, piv, comp)};
      std::distance(begin, mid) < kCutoff) {
    std::sort(begin, mid, comp);
    sort(mid, end, comp);
  } else {
    // Submit LHS to the pool while current thread continues operating on RHS
    auto futL{submit([=, this] { this->sort(begin, mid, comp); })};
    sort(mid, end, comp);
    futL.wait();
  }
}

inline void ThreadPool::workerLoop(const Index threadId) {
  // Store information related to the worker so that it can write to its own
  // deque if submissions happen "recursively" (i.e., a pushed task submits new
  // tasks)
  currentPool = this;
  workerId = threadId;

  while (true) {
    // Try executing a task from the worker's deque or stealing another
    // worker's task
    if (tryExecuteTask(threadId)) {
      continue;
    }

    // Review current state of pool (relaxed is sufficient for this reading
    // any decision to actually sleep is guarded by the secondary seq_cst
    // check)
    const auto [_, nQueuedTasks, stop]{getState(MemoryOrder::relaxed)};

    // Destructor called and we should break if no tasks remain
    if (stop) [[unlikely]] {
      const Index nPending{nPendingTasks_.load(MemoryOrder::acquire)};

      // All tasks are done, go ahead and break (acquire synchronize with
      // release decrements which happen after tasks complete. This ensures
      // workers don't finish before all tasks are done. This ensurement
      // prevents instances where workers terminate except for one thread,
      // that thread performs a task that pushes new tasks and then is stuck
      // performing them sequentially
      if (nPending == 0) {
        break;
      }

      // If there are non-executing tasks remaining, we loop and try again for
      // tasks, otherwise, we park until workers executing tasks indicate an
      // update
      if (nQueuedTasks == 0) {
        // Listen for remaining tasks to either finish (notification happens in
        // executeTask() in which case we will try to terminate next loop) or
        // submit a new task
        nPendingTasks_.wait(nPending, MemoryOrder::relaxed);
      }
    }

    // Thread just tried to find a task but found nothing, yield if still no
    // queued tasks
    if (nQueuedTasks == 0 && !stop) {
      // Announce intention to park
      nParkedWorkers_.fetch_add(1, MemoryOrder::relaxed);

      // Sequential consistency synchronizes with submit() and prevents
      // store-load reordering that could cause a lost wakeup and indefinite
      // parking
      // Submit:       1) Marks task queued   -> 2) Checks if worker asleep
      // WorkerLoop(): 1) Marks worker parked -> 2) Checks if task is queued
      // With weaker memory orderings, both threads could see old memory at
      // step 2 and if both read 0, we get deadlock
      std::atomic_thread_fence(MemoryOrder::seq_cst);

      // Re-read state after incrementing parked counter to catch races
      const auto [expectedState, expectedTasks,
                  expectedStop]{getState(MemoryOrder::relaxed)};

      if (expectedTasks == 0 && !expectedStop) {
        // Value hasn't changed, safe to park
        state_.wait(expectedState, MemoryOrder::relaxed);
      }

      // Indicate thread is no longer parked (relaxed semantics sufficient
      // since worst case, a concurrent submit() sees nParkedWorkers_ > 0 and
      // sends an unnecessary notify_one())
      nParkedWorkers_.fetch_sub(1, MemoryOrder::relaxed);
    }
  }
}

inline bool ThreadPool::tryExecuteTask(const Index threadId) {
  TW_ASSERT(threadId >= 0 && threadId < nThreads_, "Invalid thread ID");

  // Drain the global injection (MPSC) queue into this worker's deeque so
  // other threads can steal from it
  if (const KeyGuard keyGuard{injectionKey_}; keyGuard.holdsKey()) {
    // Limit the amount a worker can drain at a time to minimize deque
    // allocations and encourage task sharing among deques
    for (Index _{0}; _ < kMaxDrain; ++_) {
      if (const std::optional task{injectionQueue_.pop()}) {
        workerDeques_[threadId].push(*task);
      } else {
        break;
      }
    }
  }

  // Try taking a task from our current thread's work queue first
  if (const std::optional task{workerDeques_[threadId].pop()}) {
    executeTask(*task);
    return true;
  }

  // Try stealing a task from the other threads (random start index alleviates
  // contention)
  const Index victimIdx{getVictim(threadId)};
  bool stoleTask{false};

  for (Index i{0}; i < nThreads_; ++i) {
    // Index of thread to try to steal from
    const Index stealId{(victimIdx + i) % nThreads_};

    // Prevent worker from stealing from itself
    if (stealId == threadId) {
      continue;
    }

    // Successful stealing of a task
    if (const std::optional task{workerDeques_[stealId].steal()}) {
      executeTask(*task);
      stoleTask = true;
      break;
    }
  }

  return stoleTask;
}

inline void ThreadPool::executeTask(Internal::TaskBase* const task) {
  TW_ASSERT(task != nullptr, "Attempted to execute a null task");
  TW_DEBUG_ONLY(
      const auto [_state, _nQueued, _stop]{getState(MemoryOrder::relaxed)};
      TW_ASSERT(_nQueued > 0, "Executed task when queued task count was 0"););

  // Decrement queued count before execution (allows idle workers to unpark
  // - relaxed semantics are fine since no data guarantees required)
  decrementNumQueued(MemoryOrder::relaxed);
  task->execute_(task);

  // Decrement total count after execution (allows workers to know when to
  // terminate worker loop - release semantics must be used to ensure
  // execution happens before acquire load ensuring no tasks being executed
  // can submit new tasks)
  if (nPendingTasks_.fetch_sub(1, MemoryOrder::release) == 1) {
    // Acquire synchronizes with release in dtor preventing case where stop
    // is an "old" value and thus a missed notify_all results in deadlock
    const auto [_1, _2, stop]{getState(MemoryOrder::acquire)};

    // After we have executed the last task and this was the last task in
    // the queue, we need to unpark all workers parked on the number of
    // pending task
    if (stop) [[unlikely]] {
      nPendingTasks_.notify_all();
    }
  }
}

inline Index ThreadPool::getVictim(const Index threadId) const noexcept {
  thread_local std::mt19937 rng{static_cast<std::size_t>(threadId)};
  std::uniform_int_distribution<Index> idxDist{0, nThreads_ - 1};
  const Index idx1{idxDist(rng)};
  const Index idx2{idxDist(rng)};

  // Try preventing the calling worker from stealing from itself first
  if (idx1 == threadId) {
    return idx2;
  }

  if (idx2 == threadId) {
    return idx1;
  }

  // Otherwise, return the index of the worker with seemingly more work
  return workerDeques_[idx1].approxSize() > workerDeques_[idx2].approxSize()
             ? idx1
             : idx2;
}

inline std::tuple<std::uint64_t, std::uint64_t, bool> ThreadPool::getState(
    const std::memory_order order) const noexcept {
  const std::uint64_t state{state_.load(order)};
  const std::uint64_t nQueuedTasks{state >> kTaskShift};
  const std::uint64_t stop{state & kStopMask};
  return std::make_tuple(state, nQueuedTasks, static_cast<bool>(stop));
}

inline void ThreadPool::setStop(const std::memory_order order) noexcept {
  state_.fetch_or(kStopMask, order);
}

inline void ThreadPool::incrementNumQueued(
    const std::memory_order order) noexcept {
  state_.fetch_add(kTaskUnit, order);
}

inline void ThreadPool::decrementNumQueued(
    const std::memory_order order) noexcept {
  TW_DEBUG_ONLY(
      // If decrementNumQueued is called when nQueuedTasks == 0, bit underflow
      // will corrupt the kStopMask bit (LSB), putting the thread pool into an
      // irrecoverable state.
      const auto [_state, _nQueued, _stop]{getState(MemoryOrder::relaxed)};
      TW_ASSERT(_nQueued > 0,
                "Underflow detected: decrementNumQueued called with 0 queued "
                "tasks"););

  state_.fetch_sub(kTaskUnit, order);
}

inline void ThreadPool::awaitTask(Internal::TaskBase* const task) {
  TW_ASSERT(task != nullptr, "Cannot await a null task");

  // Non-worker: Fall back to standard atomic parking
  if (!currentPool) {
    task->wait();
    return;
  }

  TW_ASSERT(workerId >= 0 && workerId < currentPool->nThreads_,
            "Invalid workerId for current pool");
  int pauseCnt{1};
  constexpr int kMaxPauseCnt{64};

  // Worker continues executing tasks until the target task completes
  while (!task->isReady()) {
    if (currentPool->tryExecuteTask(workerId)) {
      pauseCnt = 1;
    } else {
      // If the current thread failed to find a task to execute, exponentially
      // backoff
      for (int _{0}; _ < pauseCnt; ++_) {
        Internal::tryPause();
      }

      pauseCnt = std::min(pauseCnt << 1, kMaxPauseCnt);
    }
  }
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

inline ThreadPool::KeyGuard::KeyGuard(std::atomic_flag& key)
    : key_{key}, keyAcquired_{!key.test_and_set(MemoryOrder::acquire)} {}

inline ThreadPool::KeyGuard::~KeyGuard() {
  if (keyAcquired_) {
    key_.clear(MemoryOrder::release);
  }
}

inline bool ThreadPool::KeyGuard::holdsKey() const noexcept {
  return keyAcquired_;
}

}  // namespace ThreadWeave

#endif
