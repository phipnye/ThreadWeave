#ifndef TW_NODE_H
#define TW_NODE_H

#include <atomic>
#include <concepts>

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
 * Simple aggregate for nodes of a singly linked list to be used as the
 * underlying implementation of a stack. The additional retire next pointer
 * allows storage in a retirement list without introducing data races.
 * @tparam T Type of data to store in the node
 */
template <typename T>
struct StackNode {
  T data;
  StackNode* next;

  struct {
    StackNode* next;
    bool isBlockStart;
  } _internal;
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

  struct {
    QueueNode* next;
    bool isBlockStart;
  } _internal;
};

}  // namespace ThreadWeave::Internal

#endif
