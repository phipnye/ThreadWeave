#ifndef TW_FUTURE_H
#define TW_FUTURE_H

#include <threadweave/internal/NodeAllocator.h>
#include <threadweave/internal/Task.h>
#include <threadweave/internal/utils.h>

#include <exception>
#include <new>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

// TODO: Resolve that ThreadPool must be defined (potentially make internal)

// Forward declare ThreadPool so we can use it as a default policy type
class ThreadPool;

/**
 * A template class providing a mechanism to retrieve results from an
 * asynchronous operation
 * @tparam T A generic type indicating the return type of the asynchronous
 * operation
 * @tparam WaitPolicy A class (generally a thread pool) that defines how to
 * await a task
 */
template <typename T, typename WaitPolicy = ThreadPool>
class Future {
  // --- Data members
  using Task = Internal::Task<T>;
  using Allocator = Internal::NodeAllocator<Task>;
  Task* task_;

 public:
  // --- Ctors, dtor, and assignment operators

  /**
   * Construct a future with a node containing the necessary data to do a task
   * asynchronously
   * @param task A pointer to a future node for storing results, exceptions,
   * functions, and payloads
   */
  explicit Future(Task* task);

  /**
   * Safely return our node back to our allocator to return it to the free list
   * if it's no longer being used or save it for later if some thread is using
   * it
   */
  ~Future();

  // Prevent copies
  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;

  /**
   * Move construct a new Future instance by stealing the other future's
   * resources
   * @param other Future to move construct from
   */
  Future(Future&& other) noexcept;

  /**
   * Assign a future another future's resources
   * @param other Future to move assign from
   * @return a reference to the assigned Future
   */
  Future& operator=(Future&& other) noexcept;

  /**
   * If the caller is a thread pool worker, waits on a future result but
   * continues work while waiting. Otherwise, blocks until the result becomes
   * available.
   */
  void wait() noexcept;

  /**
   * The get member function waits (by calling wait()) until the shared state is
   * ready, then retrieves the value stored in the future's resources (if any).
   * If an exception was stored, then that exception will be thrown instead. Get
   * supports recursive thread pool workers calls. If the calling thread is a
   * thread pool worker, .get() calls Future::wait(), which routes to
   * ThreadPool::awaitNode() which detects that the caller is a worker thread.
   * Instead of blocking, the worker thread calls tryExecuteTask(workerId)
   * repeatedly.Recursion unwinds completely without any thread going to sleep,
   * making deadlock mathematically impossible regardless of thread count.
   * @return the value stored in the future
   */
  T get();

 private:
  /**
   * Retire the passed node by decrementing it's internal reference count and
   * deallocating it if the caller is the last to use it
   * @param task A pointer to the future node that is no longer needed by the
   * caller
   */
  static void retireTaskNode(Task* task);
};

template <typename T, typename WaitPolicy>
Future<T, WaitPolicy>::Future(Task* const task) : task_{task} {
  TW_ASSERT(task != nullptr, "Future ctor received a null node");
}

template <typename T, typename WaitPolicy>
Future<T, WaitPolicy>::~Future() {
  retireTaskNode(task_);
}

template <typename T, typename WaitPolicy>
Future<T, WaitPolicy>::Future(Future&& other) noexcept
    : task_{std::exchange(other.task_, nullptr)} {}

template <typename T, typename WaitPolicy>
Future<T, WaitPolicy>& Future<T, WaitPolicy>::operator=(
    Future&& other) noexcept {
  if (this != &other) {
    // Before acquiring other task, mark this future as no longer using it
    retireTaskNode(task_);
    task_ = std::exchange(other.task_, nullptr);
  }

  return *this;
}

template <typename T, typename WaitPolicy>
void Future<T, WaitPolicy>::wait() noexcept {
  TW_ASSERT(task_ != nullptr,
            "Cannot call wait() on an uninitialized, invalid, or "
            "already-consumed Future");

  // Gets routed to call thread pool's awaitTask(). This is done to give context
  // about the calling thread to determine if it's a worker. If it's a worker,
  // we want it to continue doing work so the pool does not become starved if
  // tasks submit new tasks
  WaitPolicy::awaitTask(task_);
}

template <typename T, typename WaitPolicy>
T Future<T, WaitPolicy>::get() {
  TW_ASSERT(task_ != nullptr,
            "Cannot call get() on an uninitialized, invalid, or "
            "already-consumed Future");
  wait();
  Task* const task{std::exchange(task_, nullptr)};

  // Rethrow any stored exceptions
  if (task->exception_) {
    // Steal exception pointer before deallocating and then rethrowing
    auto ex{std::move(task->exception_)};
    retireTaskNode(task);
    std::rethrow_exception(ex);
  }

  if constexpr (std::is_void_v<T>) {
    retireTaskNode(task);
    return;
  } else {
    TW_ASSERT(
        task->hasResult_,
        "Future::get() called but node has no result or exception stored");

    // Under the C++ standard (specifically [basic.life]), a new object is
    // only "transparently replaceable" (meaning you can keep using the old
    // pointer without UB) if all of the following conditions are met:
    // 1. The new object is allocated at the exact same address as the old
    // one.
    // 2. The new object is the exact same type as the old one (ignoring
    // cv-qualifiers).
    // 3. The type does not contain any const-qualified fields (at any level
    // of nesting).
    // 4. The type does not contain any reference fields (at any level of
    // nesting).
    // 5. Both the old and new objects are the most-derived object (i.e., you
    // aren't replacing a base class subobject of a larger class).
    // resultBuffer is of type std::byte[] and decays to a byte* thus launder
    // is necessary here to prevent violating 2.
    T* const resultBuffer{
        std::launder(reinterpret_cast<T*>(task->resultStorage_))};

    // Store results and free resources before returning result
    T res{std::move(*resultBuffer)};
    resultBuffer->~T();
    task->hasResult_ = false;
    retireTaskNode(task);
    return res;
  }
}

template <typename T, typename WaitPolicy>
void Future<T, WaitPolicy>::retireTaskNode(Task* const task) {
  if (task && task->releaseReference()) {
    Allocator::deallocate(task);
  }
}

}  // namespace ThreadWeave

#endif
