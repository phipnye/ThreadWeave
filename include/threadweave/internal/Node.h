#ifndef TW_NODE_H
#define TW_NODE_H

#include <threadweave/internal/utils.h>

#include <atomic>
#include <concepts>
#include <memory>
#include <new>
#include <type_traits>

namespace ThreadWeave::Internal {

/**
 * A helper aggreagate to manage "internal" data that our allocator can use to
 * resolve memory managment tasks without manipulating the node's "actual" next
 * member causing potential data races
 * @tparam Node a node type to link
 */
template <typename Node>
struct AllocatorInfo {
  Node* next{nullptr};
  bool isBlockStart{false};
};

/**
 * Shared reset logic for a plain payload member (StackNode/QueueNode's data)
 * @tparam T a generic type of data contained in a node instance
 * @param value a value type to be reset/overwritten with value initialized data
 */
template <typename T>
void resetValue(T& value) noexcept {
  static_assert(std::is_nothrow_default_constructible_v<T>,
                "Node payload type must be nothrow-default-constructible to "
                "be safely recycled inside a noexcept reset()");
  if constexpr (std::is_trivially_destructible_v<T> &&
                std::is_trivially_default_constructible_v<T> &&
                std::is_trivially_copy_assignable_v<T>) {
    value = T{};
  } else {
    value.~T();
    ::new (std::addressof(value)) T{};
  }
}

/**
 * Simple aggregate for nodes of a singly linked list to be used as the
 * underlying implementation of a stack. The additional retire next pointer
 * allows storage in a retirement list without introducing data races.
 * @tparam T Type of data to store in the node
 */
template <typename T>
  requires(std::is_nothrow_default_constructible_v<T>)
struct StackNode {
  T data{};
  StackNode* next{nullptr};
  AllocatorInfo<StackNode> _internal{};

  void reset() noexcept {
    resetValue(data);
    next = nullptr;
  }
};

/**
 * Simple aggregate for nodes of a singly linked list to be used as the
 * underlying implementation of a queue. The additional retire next pointer
 * allows storage in a retirement list without introducing data races.
 * @tparam T Type of data to store in the node
 */
template <typename T>
  requires(std::is_nothrow_default_constructible_v<T>)
struct QueueNode {
  T data{};
  std::atomic<QueueNode*> next{nullptr};
  AllocatorInfo<QueueNode> _internal{};

  void reset() noexcept {
    resetValue(data);
    next.store(nullptr, MemoryOrder::relaxed);
  }
};

// Concept to check if the type has a raw internal next pointer for internal
// node mechanics like pushing to a retirement list
template <typename Node>
concept HasAllocatorInfo = requires(Node node) {
  { node._internal } -> std::same_as<AllocatorInfo<Node>&>;
};

// Concept to check if a node has a reset method
template <typename Node>
concept HasReset = requires(Node node) {
  { node.reset() } noexcept;
};

// Concept to determine if a node type has sufficient information for the
// allocator
template <typename Node>
concept AllocatorEligibleNode = HasAllocatorInfo<Node> && HasReset<Node>;

}  // namespace ThreadWeave::Internal

#endif
