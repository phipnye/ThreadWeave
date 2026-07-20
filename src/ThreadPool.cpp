#include <threadweave/ThreadPool.h>
#include <threadweave/utils.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <vector>

namespace ThreadWeave {

ThreadPool::ThreadPool(const Index nThreads)
    : threadQueues_{std::make_unique<ChaseLevDeque<NodeBase*>[]>(nThreads)},
      globalQueue_{},
      workers_{},
      nThreads_{nThreads},
      nPendingTasks_{0},
      taskSignal_{0},
      stop_{false} {
  // Fill pool with worker threads
  workers_.reserve(nThreads);

  for (Index threadId{0}; threadId < nThreads; ++threadId) {
    workers_.emplace_back(&ThreadPool::workerLoop, this, threadId);
  }
}

ThreadPool::~ThreadPool() {
  // Indicate to the threads to stop
  stop_.store(true, MemoryOrder::release);

  // Wake every worker that's currently blocked on the semaphore
  taskSignal_.release(nThreads_);

  // Join all of the workers
  workers_.clear();
}

void ThreadPool::workerLoop(const Index threadId) {
  // Index to try to steal from (will start with the 'next' thread)
  Index stealId{threadId};

  while (true) {
    // Take tasks from global queue and move them to thread's queue
    if (auto task{globalQueue_.pop()}) {
      threadQueues_[threadId].push(*task);  // note task is trivially copyable
    }

    // Try taking a task from our current thread's work queue first
    if (auto task{threadQueues_[threadId].pop()}) {
      // Popped task is a future node, cast to base class pointer and call
      // execute function pointer data member which casts the void* node to the
      // correct templated node pointer
      NodeBase* const base{*task};
      base->execute(base);
      nPendingTasks_.fetch_sub(1, MemoryOrder::release);
      continue;
    }

    // Try stealing a task from the other threads
    bool stoleTask{false};

    for (Index _{0}; _ < nThreads_; ++_) {
      // Index of thread to try to steal from
      ++stealId;
      stealId %= nThreads_;

      // Skip current thread
      if (stealId == threadId) {
        continue;
      }

      // Successful stealing of a task
      if (auto task{threadQueues_[stealId].steal()}) {
        NodeBase* const base{*task};
        base->execute(base);
        stoleTask = true;
        nPendingTasks_.fetch_sub(1, MemoryOrder::release);
        break;
      }
    }

    if (stoleTask) {
      continue;
    }

    // Destructor set stop and there are no remaining tasks. Note that it is not
    // possible for a thread to take the last task and then other threads to
    // terminate before the last task is done due to the ordering constraints of
    // execution before decrementing. This is done purposefully to protect
    // against the last task submitting additional tasks and then the last
    // thread performing those tasks sequentially.
    if (stop_.load(MemoryOrder::acquire) &&
        nPendingTasks_.load(MemoryOrder::acquire) == 0) {
      break;
    }

    // Block until a submit() signals new work, or until the timeout expires
    taskSignal_.try_acquire_for(std::chrono::milliseconds{1});
  }
}

}  // namespace ThreadWeave
