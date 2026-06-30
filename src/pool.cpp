#include <threadweave/main.h>

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
Pool::Pool(unsigned nThreads) {
  // Make sure at least one thread
  nThreads = std::max(nThreads, 1u);
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
Pool::~Pool() {
  // Indicate to workers that they can stop
  {
    std::lock_guard lock{mutex_};
    stop_ = true;
  }

  cv_.notify_all();

  // Join threads
  for (auto& t : workers_) {
    t.join();
  }
}

}  // namespace ThreadWeave
