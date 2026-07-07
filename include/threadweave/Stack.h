#ifndef TW_STACK_H
#define TW_STACK_H

#include <threadweave/Hazard.h>
#include <threadweave/Node.h>
#include <threadweave/RetireList.h>
#include <threadweave/StackOps.h>
#include <threadweave/ThreadPool.h>

#include <atomic>
#include <optional>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

/**
 * An implementation of a lock-free (Treiber) stack data structure
 * @tparam T type of the object to store in the stack
 */
template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
class Stack {
  // --- Data members
  using Node = Internal::StackNode<T>;
  alignas(Internal::CacheLineSize) std::atomic<Node*> head_{nullptr};
  alignas(Internal::CacheLineSize) Internal::RetireList<Node*> toBeDeleted_{};

 public:
  // --- Ctors, dtor, and assignment operators

  /**
   * Default construct a Treiber stack
   */
  Stack() = default;

  /**
   * Free all memory associated with the stack
   */
  ~Stack();

  // Prevent copy and move operations
  Stack(const Stack&) = delete;
  Stack(Stack&&) = delete;
  Stack& operator=(const Stack&) = delete;
  Stack& operator=(Stack&&) = delete;

  // --- Member functions

  /**
   * Push data to the top of the stack
   * @param data data to be inserted on top of the stack
   */
  void push(T data);

  /**
   * Pop data from the top of the stack
   * @return data popped from the stack (will be std::nullopt if the stack is
   * empty)
   */
  std::optional<T> pop();

  /**
   * Check if the stack is empty.
   * @return true if the stack is empty and false otherwise
   */
  [[nodiscard]] bool empty() const;
};

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
Stack<T>::~Stack() {
  // Note that toBeDeleted_ cleans up after itself
  Internal::deleteNodes(head_.load(std::memory_order::relaxed));
}

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
void Stack<T>::push(T data) {
  Internal::stackPush<Node, &Node::next>(head_,
                                         new Node{.data = std::move(data)});
}

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
std::optional<T> Stack<T>::pop() {
  // Pointer to node on top of the stack
  Node* popNode{Internal::stackPop<Node, &Node::next>(head_)};

  // Data should be no throw move constructible and std::optional requires no
  // heap allocation so this should be safe. Note that it is safe to
  // dereference popNode at this point. Only the current thread could've
  // gotten out of the CAS loop with popNode unlinked from the list. Thus,
  // while other threads may have a pointer pointing to same memory location,
  // they are stuck in the CAS loop and only this thread can delete it or add
  // it to the retire list which happens after this operation.
  std::optional<T> res{popNode ? std::make_optional(std::move(popNode->data))
                               : std::nullopt};

  if (popNode) {
    // Save the popped node for later if other threads are using it, otherwise
    // delete it immediately
    if (Internal::anyThreadsUsingNode(popNode)) {
      toBeDeleted_.saveForLater(popNode);
    } else {
      delete popNode;
    }

    // Try deleting any nodes we previously saved for later
    toBeDeleted_.deleteNodesChecked();
  }

  return res;
}

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
bool Stack<T>::empty() const {
  return head_.load(std::memory_order::relaxed) == nullptr;
}

}  // namespace ThreadWeave

#endif
