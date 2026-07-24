#ifndef TW_THREAD_POOL_H
#define TW_THREAD_POOL_H

#include <threadweave/ChaseLevDeque.h>
#include <threadweave/Future.h>
#include <threadweave/NodeAllocator.h>
#include <threadweave/VyukovQueue.h>
#include <threadweave/utils.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ThreadWeave {

/**
 * An implementation of a thread pool class built upon Vyukov MPSC queues of
 * tasks for non-worker threads to submit to and thread-specific Chase-Lev
 * Deques with the capability for threads to steal tasks from one another.
 */
class ThreadPool {
  enum class ThreadStatus : std::int8_t { active, yielded, finished };
  using FutureNodeBase = Internal::FutureNodeBase;

  // Helper struct of padded atomic counters to prevent false sharing
  struct alignas(Internal::CacheLineSize) ParkDetails {
    // Value only ever gets incremented and the raw value doesn't matter
    std::atomic<Index> value{std::numeric_limits<Index>::min()};
    std::atomic<bool> parked{false};
  };

  // Thread-local information that keeps track of whether the thread submitting
  // a task is a worker in which case it can write directly to its own work
  // deque that must follow SPMC semantics
  static thread_local inline ThreadPool* currentPool{nullptr};
  static thread_local inline Index workerId{-1};

  // --- Data members
  std::unique_ptr<ChaseLevDeque<FutureNodeBase*>[]> internalDeques_;
  std::unique_ptr<VyukovQueue<FutureNodeBase*>[]> externalQueues_;
  std::vector<std::thread> workers_;
  const Index nThreads_;
  std::vector<ParkDetails> parkDetails_;
  alignas(Internal::CacheLineSize) std::atomic<Index> nFinished_;
  alignas(Internal::CacheLineSize) std::atomic<Index> externalId_;
  alignas(Internal::CacheLineSize) std::atomic<bool> stop_;

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

  /**
   * Try to awake a thread
   * @param threadId the index/id of the thread to try to awaken
   */
  void tryUnparkThread(Index threadId) noexcept;

  /**
   * Put a thread to sleep
   * @param threadId the index/id of the thread to put to sleep
   * @param observedCnt
   */
  void parkThread(Index threadId, Index observedCnt) noexcept;

  /**
   * Activate a thread marking its status as active and reincrementing the
   * number of active threads if it was previously marked as done
   * @param status a reference to the prior status
   */
  void markActiveThread(ThreadStatus& status) noexcept;

  /**
   * Mark a thread as finished and decrement the number of active threads if it
   * was previously not finished
   * @param status  a reference to the prior status
   */
  void markFinishedThread(ThreadStatus& status) noexcept;
};

template <typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> Future<std::invoke_result_t<F, Args...>> {
  using ReturnType = std::invoke_result_t<F, Args...>;
  using Node = Internal::FutureNode<ReturnType>;
  using Allocator = Internal::NodeAllocator<Node>;
  static_assert(!std::is_reference_v<ReturnType>,
                "Reference return types are not supported directly. Return a "
                "pointer or std::reference_wrapper instead.");

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
      "Consider reducing the size of passed arguments or define the macro "
      "TW_PAYLOAD_SIZE to increase the size of the internal buffer.");
  static_assert(alignof(BoundTask) <= alignof(std::max_align_t),
                "Task's captured state requires stricter alignment than the "
                "FutureNode payload buffer guarantees.");

  // Retrive an allocation for a node (note deallocation is taken care of by the
  // future's destructor or the lambda where release() is called)
  Node* const taskNode{Allocator::allocate()};

  // Construct the callable object directly inside the node's byte payload
  static_assert(
      std::is_nothrow_move_constructible_v<BoundTask>,
      "BoundTask's move constructor may throw and cause a memory leak");
  ::new (static_cast<void*>(taskNode->payload)) BoundTask{std::move(boundTask)};

  // Bind the execution layout
  taskNode->execute = [](FutureNodeBase* base) {
    // Re-cast back to a node pointer
    Node* self{static_cast<Node*>(base)};

    // Per the standard, a new object is only "transparently replaceable"
    // (meaning you can keep using the old pointer without UB) if all of the
    // following conditions are met:
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
        self->hasResult = true;  // helps track need to call destructor
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

  // If thread submitting task is a worker, push the task directly to its own
  // work deque
  if (currentPool == this) {
    assert(workerId >= 0 && workerId < nThreads_ && "Out of bounds threadId");
    internalDeques_[workerId].push(taskNode);

    // Try unparking another thread to promote work stealing (for 1 thread
    // pools, this does needless work but it's expected to be rare)
    tryUnparkThread((workerId + 1) % nThreads_);
  } else {
    // Otherwise, use a round-robin approach to push a task to a thread's
    // Vyukov queue which supports MPSC semantics but requires the worker
    // to transfer the tasks for them to be stolen
    const Index idx{externalId_.fetch_add(1, MemoryOrder::relaxed) % nThreads_};
    assert(idx >= 0 && idx < nThreads_ && "Out of bounds threadId");
    externalQueues_[idx].push(taskNode);

    // Awake the corresponding thread if it's waiting, it must be this thread
    // as the sole consumer of the work queue (release ensures other threads see
    // pushed task preventing a dangerous wake up where a thread awakes too
    // early, looks in its external queue finds nothing, and goes back to sleep
    // before the task arrives at which point a call to get() could block
    // indefinitely waiting on a value no thread except the sleeping thread can
    // touch
    tryUnparkThread(idx);
  }

  return Future<ReturnType>{taskNode};
}

}  // namespace ThreadWeave

#endif
