#ifndef TW_FUTURE_H
#define TW_FUTURE_H

#include <threadweave/Node.h>
#include <threadweave/NodeAllocator.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

namespace Internal {

// Simple enum indicating the status a future
enum class Status : std::int8_t { pending, ready, waiting };

template <typename T>
class FutureNode {
 public:
  static constexpr Index PayLoadSize{128};  // TODO: Introduce macro to control

  // --- Data members

  // Result and exception storage
  using ResultT = std::conditional_t<std::is_void_v<T>, std::byte, T>;
  alignas(ResultT) std::byte resultBuffer_[sizeof(ResultT)]{};
  std::exception_ptr exception_{nullptr};

  // Status and function to execute
  void (*execute_)(FutureNode*){nullptr};
  alignas(std::max_align_t) std::byte payload_[PayLoadSize]{};
  std::atomic<Status> state_{Status::pending};

  // Internal data for our allocator to resolve memory management tasks
  InternalNode<FutureNode> _internal{};

  // --- Ctor, dtor, and assignment operators

  // Default ctor and dtor
  FutureNode() = default;
  ~FutureNode() = default;

  // Prevent move and copy operations
  FutureNode(const FutureNode&) = delete;
  FutureNode(FutureNode&&) = delete;
  FutureNode& operator=(const FutureNode&) = delete;
  FutureNode& operator=(FutureNode&&) = delete;

  // --- Member functions

  // Wait for the task to finish running
  void wait() noexcept {
    // Early-return if task already complete
    if (state_.load(std::memory_order::acquire) == Status::ready) {
      return;
    }

    // Try transitioning from waiting to running
    auto expected{Status::pending};
    state_.compare_exchange_strong(expected, Status::waiting);

    // Wait until the task is ready (no longer waiting)
    while (state_.load(std::memory_order::acquire) != Status::ready) {
      state_.wait(Status::waiting, std::memory_order::relaxed);
    }
  }

  // Notify when the task is done running
  void notify() noexcept {
    // Update to ready and notify waiting entities if it was originally in a
    // waiting state
    const Status oldState{
        state_.exchange(Status::ready, std::memory_order::release)};

    if (oldState == Status::waiting) {
      state_.notify_one();
    }
  }
};

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
  using Node = Internal::FutureNode<T>;
  using Allocator = Internal::NodeAllocator<Node>;
  Node* node_;

 public:
  /**
   * Construct a future with a node containing the necessary data to do a task
   * asynchronously
   * @param node A pointer to a future node for storing results, exceptions,
   * functions, and payloads
   */
  explicit Future(Node* node) : node_{node} {}

  /**
   * Safely return our node back to our allocator to return it to the free list
   * if it's no longer being used or save it for later if some thread is using
   * it
   */
  ~Future() {
    // We hand the node back to the allocator. If a worker is still running it,
    // the worker's HazardGuard will cause the allocator to defer retiring it.
    Allocator::deallocate(node_);
  }

  // Only support move-semantics
  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;

  /**
   * Move construct a new Future instance by stealing the other future's
   * resources
   * @param other Future to move construct from
   */
  Future(Future&& other) noexcept
      : node_{std::exchange(other.node_, nullptr)} {}

  /**
   * Assign a future another future's resources
   * @param other Future to move assign from
   * @return a reference to the assigned Future
   */
  Future& operator=(Future&& other) noexcept {
    if (this != &other) {
      // Return the node to the allocator before exchanging resources
      Allocator::deallocate(node_);
      node_ = std::exchange(other.node_, nullptr);
    }

    return *this;
  }

  /**
   * Blocks until the result becomes available
   */
  void wait() noexcept {
    node_->wait();  // assume non-null
  }

  /**
   * The get member function waits (by calling wait()) until the shared state is
   * ready, then retrieves the value stored in the future's resources (if any).
   * If an exception was stored, then that exception will be thrown instead
   * @return the value stored in the future
   */
  T get() {
    // Wait until the result is ready
    wait();

    // Steal the future node
    Node* node{std::exchange(node_, nullptr)};  // assume non-null

    // Rethrow any stored exceptions
    if (auto& exception{node->exception_}) {
      // Steal exception pointer before deallocating and then rethrowing
      auto ex{std::move(exception)};
      Allocator::deallocate(node);
      std::rethrow_exception(ex);
    }

    if constexpr (std::is_void_v<T>) {
      Allocator::deallocate(node);
      return;  // silences IDE
    } else {
      // Store results and free resources before returning result
      T* resultBuffer{reinterpret_cast<T*>(node->resultBuffer_)};
      T res{std::move(*resultBuffer)};
      resultBuffer->~T();
      Allocator::deallocate(node);
      return res;
    }
  }
};

}  // namespace ThreadWeave

#endif
