#ifndef TW_POOL_H
#define TW_POOL_H

#include <threadweave/ChaseLevDeque.h>
#include <threadweave/Constants.h>

#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ThreadWeave {

namespace Internal {

// A simple wrapper around a function pointer
struct Task {
  void (*fun)(){nullptr};

  void operator()() const {
    fun();  // assumes non-null
  }
};

}  // namespace Internal

class ThreadPool {
  // --- Data members
  using Task = Internal::Task;
  std::unique_ptr<ChaseLevDeque<Task>[]> queues_;
  std::vector<std::jthread> workers_;
  std::atomic<Index> runningId_;
  const Index nThreads_;
  std::atomic<bool> stop_;

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

}

}  // namespace ThreadWeave

#endif
