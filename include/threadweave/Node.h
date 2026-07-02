#ifndef TW_NODE_H
#define TW_NODE_H

#include <atomic>
#include <concepts>

namespace ThreadWeave::Internal {

// Concept to check if the type has a raw next pointer
template <typename T>
concept HasRawNextPointer = requires(T t) {
  { t.next } -> std::same_as<T*&>;
};

template <typename T>
concept HasAtomicNextPointer = requires(T t) {
  { t.next } -> std::same_as<std::atomic<T*>&>;
};

// Simple aggregate for nodes of a singly linked list
template <typename T>
struct SinglyLinkedListNode {
  T data;
  SinglyLinkedListNode* next;
};

// Simple aggregate for nodes of a singly linked list with atomic next pointers
template <typename T>
struct AtomicSinglyLinkedListNode {
  T data;
  std::atomic<AtomicSinglyLinkedListNode*> next;
};

// Delete all the nodes in a singly linked list
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
