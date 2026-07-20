#ifndef TW_FUTURE_H
#define TW_FUTURE_H

#include <threadweave/Node.h>
#include <threadweave/NodeAllocator.h>

#include <cassert>
#include <exception>
#include <new>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

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
  // --- Ctors, dtor, and assignment operators

  /**
   * Construct a future with a node containing the necessary data to do a task
   * asynchronously
   * @param node A pointer to a future node for storing results, exceptions,
   * functions, and payloads
   */
  explicit Future(Node* node);

  /**
   * Safely return our node back to our allocator to return it to the free list
   * if it's no longer being used or save it for later if some thread is using
   * it
   */
  ~Future();

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
  Future& operator=(Future&& other) noexcept;

  /**
   * Blocks until the result becomes available
   */
  void wait() noexcept;

  /**
   * The get member function waits (by calling wait()) until the shared state is
   * ready, then retrieves the value stored in the future's resources (if any).
   * If an exception was stored, then that exception will be thrown instead
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
  static void retire(Node* node);
};

template <typename T>
Future<T>::Future(Node* node) : node_{node} {}

template <typename T>
Future<T>::~Future() {
  retire(node_);
}

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
  assert(node_ && "Waiting on null node");
  node_->wait();
}

template <typename T>
T Future<T>::get() {
  // Wait until the result is ready
  wait();

  // Steal the future node
  assert(node_ && "Called get on a null future node");
  Node* node{std::exchange(node_, nullptr)};

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
void Future<T>::retire(Node* node) {
  if (node && node->release()) {
    Allocator::deallocate(node);
  }
}

}  // namespace ThreadWeave

#endif
