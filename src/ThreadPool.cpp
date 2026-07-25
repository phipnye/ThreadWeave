#include <threadweave/ThreadPool.h>
#include <threadweave/VyukovQueue.h>
#include <threadweave/utils.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <random>
#include <tuple>
#include <vector>

namespace ThreadWeave {

ThreadPool::ThreadPool(const Index nThreads)
    : workerDeques_{
          std::make_unique<ChaseLevDeque<FutureNodeBase*>[]>(nThreads)},
      injectionQueue_{},
      workers_{},
      nThreads_{nThreads},
      state_{0},
      nPendingTasks_{0},
      nParkedWorkers_{0},
      injectionKey_{} {
  // Fill pool with worker threads
  assert(nThreads > 0 && "Number of threads must be positive");
  workers_.reserve(nThreads);

  try {
    for (Index threadId{0}; threadId < nThreads; ++threadId) {
      workers_.emplace_back(&ThreadPool::workerLoop, this, threadId);
    }
  } catch (...) {
    // In case spawning a thread throws, clean up right away
    setStop(MemoryOrder::relaxed);
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
  setStop(MemoryOrder::relaxed);
  state_.notify_all();

  // Join all of the workers
  for (auto& t : workers_) {
    t.join();
  }
}

void ThreadPool::workerLoop(const Index threadId) {
  // Store information related to the worker so that it can write to its own
  // deque if submissions happen "recursively" (i.e., a pushed task submits new
  // tasks)
  currentPool = this;
  workerId = threadId;

  // Random number generation to try to steal from other thread's work deques
  std::mt19937 rng{static_cast<std::size_t>(threadId)};
  std::uniform_int_distribution<Index> idxDist{0, nThreads_ - 1};

  while (true) {
    // Drain the global injection (MPSC) queue into this worker's deeque so
    // other threads can steal from it
    if (const KeyGuard keyGuard{injectionKey_}; keyGuard.holdsKey()) {
      // Limit the amount a worker can drain at a time to minimize deque
      // allocations and encourage task sharing among deques
      for (Index _{0}; _ < 32; ++_) {
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
      continue;
    }

    // Try stealing a task from the other threads (random start index alleviates
    // contention)
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

    if (stoleTask) {
      continue;
    }

    // Review current state of pool
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
      // tasks, otherwise, we park
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
      // TODO: Review sequential consistency requirement
      nParkedWorkers_.fetch_add(1, MemoryOrder::seq_cst);

      // Re-read state AFTER incrementing parked counter to catch races
      const auto [expectedState, expectedTasks,
                  expectedStop]{getState(MemoryOrder::seq_cst)};

      if (expectedTasks == 0 && !expectedStop) {
        // Value hasn't changed, safe to park
        state_.wait(expectedState, MemoryOrder::relaxed);
      }

      nParkedWorkers_.fetch_sub(1, MemoryOrder::relaxed);
    }
  }
}

void ThreadPool::executeTask(FutureNodeBase* const task) {
  // Decrement queued count before execution (allows idle workers to unpark -
  // relaxed semantics are fine since no data guarantees required)
  decrementNumQueued(MemoryOrder::relaxed);
  task->execute(task);

  // Decrement total count after execution (allows workers to know when to
  // terminate worker loop - release semantics must be used to ensure execution
  // happens before acquire load ensuring no tasks being executed can submit new
  // tasks)
  if (nPendingTasks_.fetch_sub(1, MemoryOrder::release) == 1) {
    const auto [_1, _2, stop]{getState(MemoryOrder::relaxed)};

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
  const std::uint64_t nQueuedTasks{state >> taskShift};
  const std::uint64_t stop{state & stopMask};
  return std::make_tuple(state, nQueuedTasks, static_cast<bool>(stop));
}

void ThreadPool::setStop(const std::memory_order order) noexcept {
  state_.fetch_or(stopMask, order);
}

void ThreadPool::incrementNumQueued(const std::memory_order order) noexcept {
  state_.fetch_add(taskUnit, order);
}

void ThreadPool::decrementNumQueued(const std::memory_order order) noexcept {
  state_.fetch_sub(taskUnit, order);
}

}  // namespace ThreadWeave
