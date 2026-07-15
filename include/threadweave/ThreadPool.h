#ifndef TW_THREAD_POOL_H
#define TW_THREAD_POOL_H

#include <threadweave/ChaseLevDeque.h>
#include <threadweave/Constants.h>
#include <threadweave/Future.h>

#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ThreadWeave {

class ThreadPool {
  // --- Data members
  std::unique_ptr<ChaseLevDeque<void*>[]> queues_;
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

  template <class F, class... Args>
  auto submit(F&& f, Args&&... args)
      -> Future<std::invoke_result_t<F, Args...>>;

 private:
  void workerLoop(Index threadId);
};

template <typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> Future<std::invoke_result_t<F, Args...>> {
  using ReturnType = std::invoke_result_t<F, Args...>;
  using Node = Internal::FutureNode<ReturnType>;
  using Allocator = Internal::NodeAllocator<Node>;

  // Package the functions and arguments into a lambda
  auto boundTask{
      [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
        return std::invoke(std::move(f), std::move(args)...);
      }};
  using BoundTask = decltype(boundTask);
  static_assert(sizeof(BoundTask) <= Node::PayLoadSize,
                "Task arguments exceed the FutureNode's internal buffer "
                "limit.");  // TODO: Update to more informative message maybe
                            // suggesting how to resolve

  // Retrive an allocation for a node
  Node* node{Allocator::allocate()};

  // Reset node state
  node->state_.store(Internal::Status::pending, std::memory_order::relaxed);
  node->exception_ = nullptr;

  // Construct the callable object directly inside the node's byte payload
  ::new (static_cast<void*>(node->payload_)) BoundTask{std::move(boundTask)};

  // Bind the execution layout
  node->execute_ = [](Node* self) {
    auto* task{reinterpret_cast<BoundTask*>(self->payload_)};

    if constexpr (std::is_void_v<ReturnType>) {
      try {
        (*task)();
      } catch (...) {
        self->exception_ = std::current_exception();
      }
    } else {
      try {
        ::new (static_cast<void*>(self->resultBuffer_)) ReturnType{(*task)()};
      } catch (...) {
        self->exception_ = std::current_exception();
      }
    }

    // Explicitly clean up the bound lambda and arguments and then notify thread
    // of completion
    task->~BoundTask();
    self->notify();
  };

  // Push to worker queue
  const Index workerId{runningId_.fetch_add(1, std::memory_order::relaxed) %
                       nThreads_};
  queues_[workerId].push(static_cast<void*>(node));
  return Future<ReturnType>{node};
}

}  // namespace ThreadWeave

#endif
