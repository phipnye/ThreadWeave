#ifndef TW_STACK_OPS_H
#define TW_STACK_OPS_H

#include <threadweave/Hazard.h>

#include <atomic>

namespace ThreadWeave::Internal {

/**
 */

/**
 * Lock-free push to the top of a stack
 * @tparam Node A singly linked list Node type
 * @tparam NextPtr A data member indicating which next pointer we should link
 * (link to retire or regular next)
 * @param head Head to a singly linked list serving as the underlying stack
 * implementation
 * @param pushNode Node the push to the top of the stack
 */
template <typename Node, Node* Node::* NextPtr>
void stackPush(std::atomic<Node*>& head, Node* pushNode) {
  pushNode->*NextPtr = head.load(std::memory_order::relaxed);
  while (!head.compare_exchange_weak(pushNode->*NextPtr, pushNode,
                                     std::memory_order::release,
                                     std::memory_order::relaxed));
}

/**
 * Lock-free pop from the top of a stack
 * @tparam Node A singly linked list Node type
 * @tparam NextPtr A data member indicating which next pointer we should use
 * for popping (retire or regular next)
 * @param head Head to a singly linked list serving as the underlying stack
 * implementation
 * @return Node to the top of the stack just popped (or null if stack was empty)
 */
template <typename Node, Node* Node::* NextPtr>
Node* stackPop(std::atomic<Node*>& head) {
  // Use an RAII guard for clearing hazard pointer when node is no longer in
  // use
  const HazardGuard<0> headGuard{};
  Node* popNode{nullptr};

  do {
    // Acquire pointer pointing to head node with a hazard pointer indicating
    // current thread's use
    popNode = headGuard.acquirePointerWithHazard(head);
  } while (popNode &&
           !head.compare_exchange_strong(popNode, popNode->*NextPtr,
                                         std::memory_order::acquire,
                                         std::memory_order::relaxed));

  return popNode;
}

}  // namespace ThreadWeave::Internal

#endif
