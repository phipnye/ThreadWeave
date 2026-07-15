#include <threadweave/Constants.h>
#include <threadweave/Hazard.h>
#include <threadweave/ThreadPool.h>

#include <atomic>
#include <future>
#include <memory>
#include <thread>
#include <vector>

namespace ThreadWeave {

// Ctor
ThreadPool::ThreadPool(const Index nThreads)
    : queues_{std::make_unique<ChaseLevDeque<void*>[]>(nThreads)},
      workers_{},
      runningId_{0},
      nThreads_{nThreads},
      stop_{false} {
  // Fill pool with worker threads
  workers_.reserve(nThreads);

  for (Index threadId{0}; threadId < nThreads; ++threadId) {
    workers_.emplace_back(&ThreadPool::workerLoop, this, threadId);
  }
}

// Dtor
ThreadPool::~ThreadPool() {
  // Indicate to the threads to stop
  stop_.store(true, std::memory_order::relaxed);

  // Join all of the workers
  workers_.clear();
}

void ThreadPool::workerLoop(const Index threadId) {
  // Once destructor is stopped, we want to check one more time to make sure
  // there are no more tasks remaining
  bool checkedAgain{false};

  // Index to try to steal from (will start with the 'next' thread)
  Index stealId{threadId};

  while (true) {
    // Try taking a task from our current thread's work queue first
    if (auto task{queues_[threadId].pop()}) {
      const Internal::HazardGuard<0> hzrdGuard{};
      std::atomic<void*> atomicTask{*task};

      // Acquire and register the node pointer as hazard-active
      if (void* rawNode{hzrdGuard.acquirePointerWithHazard(atomicTask)}) {
        auto* node = static_cast<Internal::FutureNode<void>*>(rawNode);
        node->execute_(node);
      }

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
      if (auto task{queues_[stealId].steal()}) {
        const Internal::HazardGuard<0> hzrdGuard{};
        std::atomic<void*> atomicTask{*task};

        // Acquire and register the node pointer as hazard-active
        if (void* rawNode{hzrdGuard.acquirePointerWithHazard(atomicTask)}) {
          auto* node = static_cast<Internal::FutureNode<void>*>(rawNode);
          node->execute_(node);
        }

        stoleTask = true;
        break;
      }
    }

    // Only when there were no recovered tasks
    if (stoleTask) {
      continue;
    }

    // Destructor set stop, do one more pass making sure there are no
    // remaining tasks before terminating
    if (stop_.load(std::memory_order::relaxed)) {
      if (!checkedAgain) {
        checkedAgain = true;
        continue;
      }

      break;
    }

    // TODO: Update this
    std::this_thread::yield();
  }
}

}  // namespace ThreadWeave
