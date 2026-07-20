#ifndef TW_THREAD_POOL_H
#define TW_THREAD_POOL_H

#include <threadweave/ChaseLevDeque.h>
#include <threadweave/Future.h>
#include <threadweave/NodeAllocator.h>
#include <threadweave/Queue.h>
#include <threadweave/utils.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ThreadWeave {

/**
 * An implementation of a thread pool class built upon a lock-free Michael-Scott
 * queue of tasks shared by all threads and thread-local Chase-Lev Deques with
 * the capability for threads to steal tasks from one another.
 */
class ThreadPool {
  using NodeBase = Internal::FutureNodeBase;

  // --- Data members
  std::unique_ptr<ChaseLevDeque<NodeBase*>[]> threadQueues_;
  Queue<NodeBase*> globalQueue_;
  std::vector<std::jthread> workers_;
  const Index nThreads_;
  std::atomic<Index> nPendingTasks_;
  std::counting_semaphore<> taskSignal_;
  std::atomic<bool> stop_;

 public:
  // --- Ctors, Assignment, and Dtor

  /**
   * Spawn a thread pool with the specified number of threads. Defaults to
   * std::thread::hardware_concurrency() if unspecified.
   * @param nThreads a positive number of workers to spawn
   */
  explicit ThreadPool(Index nThreads = std::thread::hardware_concurrency());

  // Prevent copy and move ops
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  /**
   * Finish all remaining tasks submitted to the thread pool and join the
   * workers
   */
  ~ThreadPool();

  // --- Member functions

  /**
   * Submit a task to the thread pool
   * @tparam F A generic type of function
   * @tparam Args A generic type of arguments passed to F
   * @param f Function to call with args
   * @param args Arguments passed to f
   * @return A future instance with the result returned by f(args) or an
   * exception if one was thrown
   */
  template <class F, class... Args>
  auto submit(F&& f, Args&&... args)
      -> Future<std::invoke_result_t<F, Args...>>;

 private:
  /**
   * Loop for threads to execute while the thread pool is active
   * @param threadId the unique identifier/index of the thread indicating the
   * index of its resources like its Chase-Lev deque
   */
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

  // Future node uses an internal buffer to store function payload that is
  // aligned using max_align_t, ensure the passed task doesn't violate size
  // constraints for this buffer
  using BoundTask = decltype(boundTask);
  static_assert(
      sizeof(BoundTask) <= Node::payloadSize,
      "Task arguments exceed the FutureNode's internal buffer limit. "
      "Considering reducing the size of passed arguments or define the macro "
      "TW_PAYLOAD_SIZE to increase the size of the internal buffer.");
  static_assert(alignof(BoundTask) <= alignof(std::max_align_t),
                "Task's captured state requires stricter alignment than the "
                "FutureNode payload buffer guarantees.");

  // Retrive an allocation for a node
  Node* node{Allocator::allocate()};

  // Construct the callable object directly inside the node's byte payload
  ::new (static_cast<void*>(node->payload)) BoundTask{std::move(boundTask)};

  // Bind the execution layout
  node->execute = [](NodeBase* base) {
    // Re-cast back to a node pointer
    Node* self{static_cast<Node*>(base)};

    // Under the C++ standard (specifically [basic.life]), a new object is only
    // "transparently replaceable" (meaning you can keep using the old pointer
    // without UB) if all of the following conditions are met:
    // 1. The new object is allocated at the exact same address as the old one.
    // 2. The new object is the exact same type as the old one (ignoring
    // cv-qualifiers).
    // 3. The type does not contain any const-qualified fields (at any level of
    // nesting).
    // 4. The type does not contain any reference fields (at any level of
    // nesting).
    // 5. Both the old and new objects are the most-derived object (i.e., you
    // aren't replacing a base class subobject of a larger class).
    // Payload is of type std::byte[] and decays to a byte* thus launder is
    // necessary here to prevent violating 2.
    auto* task{std::launder(reinterpret_cast<BoundTask*>(self->payload))};

    if constexpr (std::is_void_v<ReturnType>) {
      try {
        (*task)();
      } catch (...) {
        self->exception = std::current_exception();
      }
    } else {
      try {
        ::new (static_cast<void*>(self->resultBuffer)) ReturnType{(*task)()};
        self->hasResult = true;
      } catch (...) {
        self->exception = std::current_exception();
      }
    }

    // Explicitly clean up the bound lambda and arguments and then notify thread
    // of completion
    task->~BoundTask();
    self->notify();

    // Decrement node's internal refernce count and deallocate node if caller is
    // last one holding a reference
    if (self->release()) {
      Allocator::deallocate(self);
    }
  };

  // Push tasks to global queue so threads can later take from it (note push
  // synchronizes with pop and hence relaxed memory ordering suffices here)
  nPendingTasks_.fetch_add(1, MemoryOrder::relaxed);
  globalQueue_.push(static_cast<NodeBase*>(node));
  taskSignal_.release();
  return Future<ReturnType>{node};
}

}  // namespace ThreadWeave

#endif
