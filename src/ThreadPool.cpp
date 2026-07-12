#include <threadweave/Constants.h>
#include <threadweave/ThreadPool.h>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ThreadWeave {

// Ctor
ThreadPool::ThreadPool(Index nThreads) {
  // Make sure at least one thread
  nThreads = std::max(nThreads, static_cast<Index>(1));
  workers_.reserve(nThreads);

  // Fill pool with worker threads
  for (unsigned _{0}; _ < nThreads; ++_) {
    workers_.emplace_back([this] {
      while (true) {
        // Current task for the given thread
        Task task{};

        {
          // Listen for tasks per conditional variable
          std::unique_lock lock{mutex_};
          cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

          // Only stop when all tasks have been completed
          if (stop_ && tasks_.empty()) {
            return;
          }

          // Take task from front of queue
          task = std::move(tasks_.front());
          tasks_.pop();
        }

        task();
      }
    });
  }
}

// Dtor
ThreadPool::~ThreadPool() {
  // Indicate to workers that they can stop
  {
    std::lock_guard lock{mutex_};
    stop_ = true;
  }

  cv_.notify_all();
}

}  // namespace ThreadWeave
