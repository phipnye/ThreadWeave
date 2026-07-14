#include <threadweave/Constants.h>
#include <threadweave/ThreadPool.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace ThreadWeave {

// Ctor
ThreadPool::ThreadPool(Index nThreads)
    : queues_{std::make_unique<ChaseLevDeque<Task>[]>(nThreads)},
      workers_{},
      runningId_{0},
      nThreads_{nThreads},
      stop_{false} {
  // Fill pool with worker threads
  for (Index i{0}; i < nThreads; ++i) {
    workers_.reserve(nThreads);

    workers_.emplace_back([this, i, nThreads] {
      // Distribution to randomly choose what queue to try to steal from first
      std::mt19937 rng{std::random_device{}()};
      std::uniform_int_distribution<Index> dist{0, nThreads - 1};

      // Once destructor is stopped, we want to check one more time to make sure
      // there are no more tasks remaining
      bool checkedAgain{false};

      while (true) {
        // Try taking a task from our current thread's work queue first
        if (const auto task{queues_[i].pop()}) {
          (*task)();
          continue;
        }

        // Try stealing a task from the other threads
        const Index start{dist(rng)};
        bool stoleTask{false};

        for (Index t{0}; t < nThreads; ++t) {
          const Index tId{(start + t) % nThreads};

          // Skip current thread
          if (tId == i) {
            continue;
          }

          // Successful stealing of a task
          if (const auto task{queues_[tId].steal()}) {
            (*task)();
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

        // TO DO: Is this a good choice here?
        std::this_thread::yield();
      }
    });
  }
}

// Dtor
ThreadPool::~ThreadPool() {
  // Indicate to the threads to stop
  stop_.store(true, std::memory_order::relaxed);

  // Join all of the workers
  workers_.clear();
}

}  // namespace ThreadWeave
