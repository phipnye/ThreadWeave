#include <threadweave/ThreadPool.h>
#include <threadweave/VyukovQueue.h>
#include <threadweave/internal/Task.h>
#include <threadweave/internal/utils.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <random>
#include <tuple>
#include <vector>

namespace ThreadWeave {

ThreadPool::ThreadPool(const Index nThreads)
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

ThreadPool::~ThreadPool() {
  // Indicate to the threads to stop
  setStop(MemoryOrder::release);
  state_.notify_all();

  // Join all of the workers
  for (auto& t : workers_) {
    t.join();
  }

#ifndef TW_NDEBUG
  {
    const auto [_state, _nQueued, _stop]{getState(MemoryOrder::relaxed)};
    TW_ASSERT(_nQueued == 0,
              "Thread pool destroyed with unexecuted queued tasks remaining");
    TW_ASSERT(nPendingTasks_.load(MemoryOrder::relaxed) == 0,
              "Thread pool destroyed with pending tasks remaining");
  }
#endif
}

void ThreadPool::workerLoop(const Index threadId) {
  // Store information related to the worker so that it can write to its own
  // deque if submissions happen "recursively" (i.e., a pushed task submits new
  // tasks)
  currentPool = this;
  workerId = threadId;

  while (true) {
    // Try executing a task from the worker's deque or stealing another worker's
    // task
    if (tryExecuteTask(threadId)) {
      continue;
    }

    // Review current state of pool (relaxed is sufficient for this reading any
    // decision to actually sleep is guarded by the secondary seq_cst check)
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
      // Announce intention to park (note sequential consistency synchronizes
      // with submit() and prevents store-load reordering that could cause a
      // lost wakeup and indefinite parking
      // Submit:       1) Marks task queued   -> 2) Checks if worker asleep
      // WorkerLoop(): 1) Marks worker parked -> 2) Checks if task is queued
      // With weaker memory orderings, both threads could see old memory at
      // step 2 and if both read 0, we get deadlock
      nParkedWorkers_.fetch_add(1, MemoryOrder::seq_cst);

      // Re-read state AFTER incrementing parked counter to catch races
      const auto [expectedState, expectedTasks,
                  expectedStop]{getState(MemoryOrder::seq_cst)};

      if (expectedTasks == 0 && !expectedStop) {
        // Value hasn't changed, safe to park
        state_.wait(expectedState, MemoryOrder::relaxed);
      }

      // Indicate thread is no longer parked (relaxed semantics sufficient since
      // worst case, a concurrent submit() sees nParkedWorkers_ > 0 and sends an
      // unnecessary notify_one())
      nParkedWorkers_.fetch_sub(1, MemoryOrder::relaxed);
    }
  }
}

void ThreadPool::awaitTask(Internal::TaskBase* const task) {
  TW_ASSERT(task != nullptr, "Cannot await a null task");

  // Non-worker: Fall back to standard atomic parking
  if (!currentPool) {
    task->wait();
    return;
  }

  TW_ASSERT(workerId >= 0 && workerId < currentPool->nThreads_,
            "Invalid workerId for current pool");

  // Worker continues executing tasks until the target task completes
  while (!task->isReady()) {
    // Yield if failed to find a task to execute
    if (!currentPool->tryExecuteTask(workerId)) {
      // TODO: Consider re-working this yield
      std::this_thread::yield();
    }
  }
}

bool ThreadPool::tryExecuteTask(const Index threadId) {
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
  thread_local std::mt19937 rng{static_cast<std::size_t>(threadId)};
  std::uniform_int_distribution<Index> idxDist{0, nThreads_ - 1};
  bool stoleTask{false};
  const Index victimIdx{idxDist(rng)};

  for (Index i{0}; i < nThreads_; ++i) {
    // Index of thread to try to steal from
    const Index stealId{(victimIdx + i) % nThreads_};

    // Prevent stealing worker from stealing from itself
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

void ThreadPool::executeTask(Internal::TaskBase* const task) {
  TW_ASSERT(task != nullptr, "Attempted to execute a null task");
  TW_DEBUG_ONLY(
      const auto [_state, _nQueued, _stop]{getState(MemoryOrder::relaxed)};
      TW_ASSERT(_nQueued > 0, "Executed task when queued task count was 0"););

  // Decrement queued count before execution (allows idle workers to unpark -
  // relaxed semantics are fine since no data guarantees required)
  decrementNumQueued(MemoryOrder::relaxed);
  task->execute_(task);

  // Decrement total count after execution (allows workers to know when to
  // terminate worker loop - release semantics must be used to ensure execution
  // happens before acquire load ensuring no tasks being executed can submit new
  // tasks)
  if (nPendingTasks_.fetch_sub(1, MemoryOrder::release) == 1) {
    // Acquire synchronizes with release in dtor preventing case where stop is
    // an "old" value and thus a missed notify_all results in deadlock
    const auto [_1, _2, stop]{getState(MemoryOrder::acquire)};

    // After we have executed the last task and this was the last task in the
    // queue, we need to unpark all workers parked on the number of pending task
    if (stop) [[unlikely]] {
      nPendingTasks_.notify_all();
    }
  }
}

std::tuple<std::uint64_t, std::uint64_t, bool> ThreadPool::getState(
    const std::memory_order order) const noexcept {
  const std::uint64_t state{state_.load(order)};
  const std::uint64_t nQueuedTasks{state >> kTaskShift};
  const std::uint64_t stop{state & kStopMask};
  return std::make_tuple(state, nQueuedTasks, static_cast<bool>(stop));
}

void ThreadPool::setStop(const std::memory_order order) noexcept {
  state_.fetch_or(kStopMask, order);
}

void ThreadPool::incrementNumQueued(const std::memory_order order) noexcept {
  state_.fetch_add(kTaskUnit, order);
}

void ThreadPool::decrementNumQueued(const std::memory_order order) noexcept {
#ifndef TW_NDEBUG
  {
    // If decrementNumQueued is called when nQueuedTasks == 0, bit underflow
    // will corrupt the kStopMask bit (LSB), putting the thread pool into an
    // irrecoverable state.
    const auto [_state, _nQueued, _stop]{getState(MemoryOrder::relaxed)};
    TW_ASSERT(
        _nQueued > 0,
        "Underflow detected: decrementNumQueued called with 0 queued tasks");
  }
#endif

  state_.fetch_sub(kTaskUnit, order);
}

ThreadPool::KeyGuard::KeyGuard(std::atomic_flag& key)
    : key_{key}, keyAcquired_{!key.test_and_set(MemoryOrder::acquire)} {}

ThreadPool::KeyGuard::~KeyGuard() {
  if (keyAcquired_) {
    key_.clear(MemoryOrder::release);
  }
}

bool ThreadPool::KeyGuard::holdsKey() const noexcept {
  return keyAcquired_;
}

namespace Internal {

void helpWait(TaskBase* const task) noexcept {
  ThreadPool::awaitTask(task);
}

}  // namespace Internal

}  // namespace ThreadWeave
