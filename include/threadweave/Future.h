#ifndef TW_FUTURE_H
#define TW_FUTURE_H

#include <threadweave/Node.h>
#include <threadweave/NodeAllocator.h>
#include <threadweave/utils.h>

#include <cstddef>
#include <exception>
#include <new>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

namespace Internal {
// Future node base class to hold function pointer and maintain node reference
// count
class FutureNodeBase {
 protected:
  enum class FutureStatus : std::int8_t { pending, ready, waiting };

 public:
  // --- Data members

  // Function pointer to function to execute
  void (*execute)(FutureNodeBase*){nullptr};

  // Status of the result
  std::atomic<FutureStatus> state{FutureStatus::pending};

  // Reference count for resource management (future and thread pool hold refs)
  std::atomic<std::int8_t> refCount{2};

  // --- Member functions

  // Determine if future result is ready
  bool isReady() const noexcept;

  // Decrements reference count indicating caller no longer needs the node to
  // stay alive at which point the last caller can call deallocate()
  bool release() noexcept;

  // Wait for the task to finish running
  void wait() noexcept;

  // Notify when the task is done running
  void notify() noexcept;
};

template <typename T>
class FutureNode : public FutureNodeBase {  // NOLINT(*-pro-type-member-init)
  using ResultT = std::conditional_t<std::is_void_v<T>, std::byte, T>;

 public:
#ifdef TW_PAYLOAD_SIZE
  // User-defined payload size
  static_assert(TW_PAYLOAD_SIZE > 0,
                "TW_PAYLOAD_SIZE must be strictly poisitive");
  static constexpr Index kPayloadSize{TW_PAYLOAD_SIZE};
#else
  // Default to 128 bytes if user does not define value
  static constexpr Index kPayloadSize{128};
#endif

  // --- Data members
  alignas(
      std::max_align_t) std::byte payload[kPayloadSize];  // function payload
  std::exception_ptr exception{nullptr};
  alignas(ResultT) std::byte resultBuffer[sizeof(ResultT)];
  AllocatorInfo<FutureNode> _internal{};
  bool hasResult{false};

  // Dtor
  ~FutureNode();

  // --- Member functions

  // Reset the members of our future node instance
  void reset() noexcept;

 private:
  // Clean up stored results if present
  void destroyResults() noexcept;
};

/**
 * Bridge function defined in ThreadPool.cpp to allow thread workers to continue
 * working without blocking waits while waiting for a result
 * @param node a pointer to the future node base to wait on
 */
void helpWait(FutureNodeBase* node) noexcept;

}  // namespace Internal

/**
 * A template class providing a mechanism to retrieve results from an
 * asynchronous operation
 * @tparam T A generic type indicating the return type of the asynchronous
 * operation
 */
template <typename T>
class Future {
  // --- Data members
  using FutureNode = Internal::FutureNode<T>;
  using Allocator = Internal::NodeAllocator<FutureNode>;
  FutureNode* node_;

 public:
  // --- Ctors, dtor, and assignment operators

  /**
   * Construct a future with a node containing the necessary data to do a task
   * asynchronously
   * @param node A pointer to a future node for storing results, exceptions,
   * functions, and payloads
   */
  explicit Future(FutureNode* node);

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
   * @param node A pointer to the future node that is no longer needed by the
   * caller
   */
  static void retire(FutureNode* node);
};

template <typename T>
Future<T>::Future(FutureNode* const node) : node_{node} {
  TW_ASSERT(node != nullptr, "Future ctor received a null node");
}

template <typename T>
Future<T>::~Future() {
  retire(node_);
}

template <typename T>
Future<T>::Future(Future&& other) noexcept
    : node_{std::exchange(other.node_, nullptr)} {}

template <typename T>
Future<T>& Future<T>::operator=(Future&& other) noexcept {
  if (this != &other) {
    retire(node_);
    node_ = std::exchange(other.node_, nullptr);
  }

  return *this;
}

template <typename T>
void Future<T>::wait() noexcept {
  TW_ASSERT(node_ != nullptr,
            "Cannot call wait() on an uninitialized, invalid, or "
            "already-consumed Future");
  Internal::helpWait(node_);
}

template <typename T>
T Future<T>::get() {
  TW_ASSERT(node_ != nullptr,
            "Cannot call get() on an uninitialized, invalid, or "
            "already-consumed Future");
  wait();

  // Steal the future node
  FutureNode* const node{std::exchange(node_, nullptr)};

  // Rethrow any stored exceptions
  if (node->exception) {
    // Steal exception pointer before deallocating and then rethrowing
    auto ex{std::move(node->exception)};
    retire(node);
    std::rethrow_exception(ex);
  }

  if constexpr (std::is_void_v<T>) {
    retire(node);
    return;  // silences IDE
  } else {
    TW_ASSERT(
        node->hasResult,
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
    T* resultBuffer{std::launder(reinterpret_cast<T*>(node->resultBuffer))};

    // Store results and free resources before returning result
    T res{std::move(*resultBuffer)};
    resultBuffer->~T();
    node->hasResult = false;
    retire(node);
    return res;
  }
}

template <typename T>
void Future<T>::retire(FutureNode* const node) {
  if (node && node->release()) {
    Allocator::deallocate(node);
  }
}

namespace Internal {
template <typename T>
FutureNode<T>::~FutureNode() {
  // Guards against leaking a completed result that was never retrieved
  destroyResults();
}

template <typename T>
void FutureNode<T>::reset() noexcept {
  destroyResults();
  exception = nullptr;
  execute = nullptr;
  refCount.store(2, MemoryOrder::relaxed);
  state.store(FutureStatus::pending, MemoryOrder::relaxed);
}

template <typename T>
void FutureNode<T>::destroyResults() noexcept {
  if constexpr (!std::is_void_v<T>) {
    if (hasResult) {
      std::launder(reinterpret_cast<ResultT*>(resultBuffer))->~ResultT();
      hasResult = false;
    }
  } else {
    hasResult = false;
  }
}

}  // namespace Internal

}  // namespace ThreadWeave

#endif
