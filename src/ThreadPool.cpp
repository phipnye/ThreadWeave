#include <threadweave/ThreadPool.h>
#include <threadweave/VyukovQueue.h>
#include <threadweave/utils.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <random>
#include <vector>

namespace ThreadWeave {

ThreadPool::ThreadPool(const Index nThreads)
    : workerDeques_{
          std::make_unique<ChaseLevDeque<FutureNodeBase*>[]>(nThreads)},
      injectionQueue_{},
      workers_{},
      nThreads_{nThreads},
      poolState_{0},
      nPendingTasks_{0},
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
    poolState_.fetch_or(stopMask, MemoryOrder::relaxed);
    poolState_.notify_all();

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
  poolState_.fetch_or(stopMask, MemoryOrder::relaxed);
  poolState_.notify_all();

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
    // TODO: Replace with RAII guard
    if (!injectionKey_.test_and_set(MemoryOrder::relaxed)) {
      // Limit the amount a worker can drain at a time to minimize deque
      // allocations and encourage task sharing among deques
      for (Index _{0}; _ < 32; ++_) {
        if (const std::optional task{injectionQueue_.pop()}) {
          workerDeques_[threadId].push(*task);
        } else {
          break;
        }
      }

      injectionKey_.clear(MemoryOrder::relaxed);
    }

    // Try taking a task from our current thread's work queue first
    if (const std::optional task{workerDeques_[threadId].pop()}) {
      executeTask(*task);
      continue;
    }

    // Try stealing a task from the other threads
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
    const std::uint64_t currState{poolState_.load(MemoryOrder::relaxed)};
    const std::uint64_t nQueuedTasks{currState >> taskShift};
    const std::uint64_t stop{currState & stopMask};

    // Destructor called and we should break if no tasks remain
    if (stop) [[unlikely]] {
      // All tasks are done, go ahead and break (acquire synchronize with
      // release decrements which happen after tasks complete. This ensures
      // workers don't finish before all tasks are done. This ensurement
      // prevents instances where workers terminate except for one thread,
      // that thread performs a task that pushes new tasks and then is stuck
      // performing them sequentially
      if (nPendingTasks_.load(MemoryOrder::acquire) == 0) {
        break;
      }
    }

    // Thread just tried to find a task but found nothing, yield if still no
    // queued tasks
    if (nQueuedTasks == 0 && !stop) {
      // currState == 0
      // TODO: Devise method to wait while stop signaled without burning cycles
      poolState_.wait(0, MemoryOrder::relaxed);
    } else {
      std::this_thread::yield();
    }
  }
}

void ThreadPool::executeTask(FutureNodeBase* const task) {
  // Decrement queued count before running (allows idle workers to sleep in
  // wait(0))
  poolState_.fetch_sub(taskUnit, MemoryOrder::relaxed);
  task->execute(task);

  // Decrement total count after running (preserves acquire/release teardown
  // barrier)
  nPendingTasks_.fetch_sub(1, MemoryOrder::release);
}

}  // namespace ThreadWeave
