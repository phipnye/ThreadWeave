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

// Concept to check if the type has a raw retireNext pointer
template <typename Node>
concept HasRetireNextPointer = requires(Node node) {
  { node.retireNext } -> std::same_as<Node*&>;
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
  StackNode* retireNext;
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
  QueueNode* retireNext;
};

/**
 * Delete all the nodes in a singly linked list
 * @tparam Node a generic node type for a singly linked list
 * @param list a singly linked list
 */
template <typename Node>
  requires(HasRawNextPointer<Node> || HasAtomicNextPointer<Node>)
void deleteNodes(Node* list) {
  while (list) {
    // ReSharper disable once CppLocalVariableMayBeConst
    Node* const curr{list};

    if constexpr (HasRawNextPointer<Node>) {
      list = list->next;
    } else {
      static_assert(HasAtomicNextPointer<Node>,
                    "Expected type Node to have an atomic next pointer");
      list = list->next.load(std::memory_order::relaxed);
    }

    delete curr;
  }
}

}  // namespace ThreadWeave::Internal

#endif
