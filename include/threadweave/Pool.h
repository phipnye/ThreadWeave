#ifndef TW_POOL_H
#define TW_POOL_H

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace ThreadWeave {

class Pool {
  // Type aliases
  using Task = std::move_only_function<void()>;

  // --- Data members
  std::vector<std::thread> workers_{};
  std::queue<Task> tasks_{};
  std::mutex mutex_{};
  std::condition_variable cv_{};
  bool stop_{false};

 public:
  // --- Ctors, Assignment, and Dtor

  // Ctor with user-defined number of threads
  explicit Pool(unsigned nThreads = std::thread::hardware_concurrency());

  // Remove copy and move ops
  Pool(const Pool&) = delete;
  Pool(Pool&&) = delete;
  Pool& operator=(const Pool&) = delete;
  Pool& operator=(Pool&&) = delete;

  // Dtor
  ~Pool();

  // --- Member functions

  // Place tasks into the queue so the next available thread can take on the
  // given task
  template <typename F, typename... Args>
  auto emplace(F&& f, Args&&... args)
      -> std::future<std::invoke_result_t<F, Args...>> {
    // Capture task and future storing result of task
    using ReturnType = std::invoke_result_t<F, Args...>;
    std::packaged_task<ReturnType()> task{
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)};
    std::future<ReturnType> taskFuture{task.get_future()};

    // Push task onto tasks queue and notify an available thread of the task
    {
      std::lock_guard lock{mutex_};
      tasks_.emplace([task = std::move(task)]() mutable { task(); });
    }

    cv_.notify_one();
    return taskFuture;
  }
};

}  // namespace ThreadWeave

#endif
