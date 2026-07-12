#ifndef TW_POOL_H
#define TW_POOL_H

#include <threadweave/Constants.h>

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ThreadWeave {

class ThreadPool {
  // Type aliases
  using Task = std::move_only_function<void()>;

  // --- Data members
  std::vector<std::jthread> workers_{};
  std::queue<Task> tasks_{};
  std::mutex mutex_{};
  std::condition_variable cv_{};
  bool stop_{false};

 public:
  // --- Ctors, Assignment, and Dtor

  // Ctor with user-defined number of threads
  explicit ThreadPool(Index nThreads = std::thread::hardware_concurrency());

  // Remove copy and move ops
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  // Dtor
  ~ThreadPool();

  // --- Member functions

  template <typename F, typename... Args>
  auto submit(F&& f, Args&&... args)
      -> std::future<std::invoke_result_t<F, Args...>>;
};

template <typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
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

}  // namespace ThreadWeave

#endif
