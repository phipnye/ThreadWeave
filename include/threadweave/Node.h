#ifndef TW_NODE_H
#define TW_NODE_H

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>

namespace ThreadWeave::Internal {

// Concept to check if the type has a raw next pointer
template <typename Node>
concept HasRawNextPointer = requires(Node node) {
  { node.next } -> std::same_as<Node*&>;
};

// Concept to check if the type has an atomic next pointer
template <typename Node>
concept HasAtomicNextPointer = requires(Node node) {
  { node.next } -> std::same_as<std::atomic<Node*>&>;
};

// Concept to check if the type has a raw internal next pointer for internal
// node mechanics like pushing to a retirement list
template <typename Node>
concept HasInternalNextPointer = requires(Node node) {
  { node._internal.next } -> std::same_as<Node*&>;
};

// Concept to check if the type has a boolean member isBlockStart indicating
// whether the node is the head of an allocation block
template <typename Node>
concept HasInternalBlockStartField = requires(Node node) {
  { node._internal.isBlockStart } -> std::same_as<bool&>;
};

/**
 * A helper aggreagate to manage "internal" data that our allocator can use to
 * resolve memory managment tasks without manipulating the node's "actual" next
 * member causing potential data races
 * @tparam Node a node type to link
 */
template <typename Node>
struct InternalNode {
  Node* next;
  bool isBlockStart;
};

/**
 * Simple aggregate for nodes of a singly linked list to be used as the
 * underlying implementation of a stack. The additional retire next pointer
 * allows storage in a retirement list without introducing data races.
 * @tparam T Type of data to store in the node
 */
template <typename T>
struct StackNode {
  T data;
  StackNode* next;
  InternalNode<StackNode> _internal;
};

/**
 * Simple aggregate for nodes of a singly linked list to be used as the
 * underlying implementation of a queue. The additional retire next pointer
 * allows storage in a retirement list without introducing data races.
 * @tparam T Type of data to store in the node
 */
template <typename T>
struct QueueNode {
  T data;
  std::atomic<QueueNode*> next;
  InternalNode<QueueNode> _internal;
};

}  // namespace ThreadWeave::Internal

#endif
