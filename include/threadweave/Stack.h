#ifndef TW_STACK_H
#define TW_STACK_H

#include <threadweave/Hazard.h>
#include <threadweave/Node.h>
#include <threadweave/RetireList.h>

#include <atomic>
#include <optional>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

// A thread-safe lock-free stack
template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
class Stack {
  // --- Data members
  using Node = Internal::SinglyLinkedListNode<T>;
  std::atomic<Node*> head_{nullptr};
  Internal::RetireList<Node*> toBeDeleted_{};

 public:
  // --- Ctors, dtor, and assignment operators

  // Default ctor
  Stack() = default;

  // Dtor
  ~Stack() {
    // To be executed in single-threaded context, relaxed is sufficient
    Internal::deleteNodes(head_.load(std::memory_order::relaxed));
  }

  // Prevent copy and move operations
  Stack(const Stack&) = delete;
  Stack(Stack&&) = delete;
  Stack& operator=(const Stack&) = delete;
  Stack& operator=(Stack&&) = delete;

  // --- Member functions

  // Check if the stack is lock-free
  [[nodiscard]] bool isLockFree() const {
    return head_.is_lock_free() && toBeDeleted_.isLockFree();
  }

  // Check whether the underlying atomic types are always lock-free
  [[nodiscard]] constexpr bool isAlwaysLockFree() const {
    return decltype(head_)::is_always_lock_free &&
           toBeDeleted_.isAlwaysLockFree();
  }

  // Push data onto the top of the stack
  void push(T data) {
    // Generate new node
    Node* newNode{new Node{.data = std::move(data),
                           .next = head_.load(std::memory_order::relaxed)}};

    // Continually try to assign new node as the head
    while (!head_.compare_exchange_weak(newNode->next, newNode,
                                        std::memory_order::release,
                                        std::memory_order::relaxed));
  }

  // Pop data from the top of the stack
  std::optional<T> pop() {
    // Pointer to node on top of the stack
    Node* popNode{nullptr};

    {
      // Use an RAII guard for clearing hazard pointer when node is no longer in
      // use
      Internal::HazardGuard hzrdGuard{};

      do {
        // Acquire pointer to node on top of the stack with thread's hazard
        // pointer pointing to it so that no other threads delete while acessing
        popNode = hzrdGuard.acquirePointerWithHazard(head_);
      } while (popNode &&
               !head_.compare_exchange_strong(popNode, popNode->next,
                                              std::memory_order::acquire,
                                              std::memory_order::relaxed));
    }

    // Data should be no throw move constructible and std::optional
    // requires no heap allocation so this should be safe
    std::optional<T> res{popNode ? std::make_optional(std::move(popNode->data))
                                 : std::nullopt};

    if (popNode) {
      // Save the popped node for later if other threads are using it, otherwise
      // delete it immediately
      if (!Internal::anyThreadsUsingNode(popNode)) {
        delete popNode;
      } else {
        toBeDeleted_.saveForLater(popNode);
      }

      // Try deleting any nodes we previously saved for later
      toBeDeleted_.deleteNodesChecked();
    }

    return res;
  }
};

}  // namespace ThreadWeave

#endif
