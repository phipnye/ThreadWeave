#ifndef TW_QUEUE_H
#define TW_QUEUE_H

#include <threadweave/Constants.h>
#include <threadweave/Hazard.h>
#include <threadweave/Node.h>
#include <threadweave/RetireList.h>

#include <atomic>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

/**
 * An implementation of a lock-free (Michael-Scott) queue data structure
 * @tparam T type of the object to store in the queue
 */
template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
class Queue {
  // --- Data members
  using Node = Internal::QueueNode<std::optional<T>>;
  alignas(Internal::AlignSize) std::atomic<Node*> head_;
  alignas(Internal::AlignSize) std::atomic<Node*> tail_;
  alignas(Internal::AlignSize) Internal::RetireList<Node*> toBeDeleted_{};

 public:
  // --- Ctor, dtor, and assignment operators

  // Default construct a Michael-Scott queue
  Queue()
      : head_{new Node{.data = std::nullopt,
                       .next = nullptr,
                       .retireNext = nullptr}},  // dummy
        tail_{head_.load(std::memory_order::relaxed)} {}

  // Dtor
  ~Queue() {
    // Note that toBeDeleted_ cleans up after itself
    Internal::deleteNodes(head_.load(std::memory_order::relaxed));
  }

  // Prevent copy and move operations
  Queue(const Queue&) = delete;
  Queue(Queue&&) = delete;
  Queue& operator=(const Queue&) = delete;
  Queue& operator=(Queue&&) = delete;

  // --- Member functions

  // Add data to back of queue
  void push(T data) {
    // Construct new node to store data
    Node* pushNode{new Node{
        .data = std::move(data), .next = nullptr, .retireNext = nullptr}};

    while (true) {
      // Use an RAII guard for clearing hazard pointer when held tail pointer is
      // no longer in use
      const Internal::HazardGuard hzrdGuard{0};
      Node* tailPtr{hzrdGuard.acquirePointerWithHazard(tail_)};
      std::atomic<Node*>& nextAtomic{tailPtr->next};
      Node* nextPtr{nextAtomic.load(std::memory_order::acquire)};

      // Tail is lagging behind, try moving the tail pointer chain along so that
      // we can eventually attach our new node
      if (nextPtr != nullptr) {
        tail_.compare_exchange_strong(tailPtr, nextPtr,
                                      std::memory_order::release,
                                      std::memory_order::relaxed);
      }

      // If our tail's next pointer now points to null, we can finally try
      // attaching it to the end of our list and then try moving the chain along
      else if (nextAtomic.compare_exchange_strong(nextPtr, pushNode,
                                                  std::memory_order::release,
                                                  std::memory_order::relaxed)) {
        // Try updating tail to move further down the chain (failures will get
        // resolved by later operations that push further down the chain)
        tail_.compare_exchange_weak(tailPtr, pushNode,
                                    std::memory_order::release,
                                    std::memory_order::relaxed);
        return;
      }
    }
  }

  // Get and pop data from our queue
  std::optional<T> pop() {
    // TO DO ---------------
    // Pointer to the node being popped
    Node* popNode{nullptr};

    {
      // Head is fixed dummy node so acquiring next is always safe
      std::atomic<Node*>& headNext{
          head_.load(std::memory_order::acquire)->next};

      // Use an RAII guard for clearing hazard pointer once we've acquired and
      // unlinked it from the list
      Internal::HazardGuard hzrdGuard{};

      do {
        // Acquire pointer to node at front of queue with thread's hazard
        // pointer pointing to it so that no other threads delete while acessing
        popNode = hzrdGuard.acquirePointerWithHazard(headNext);
      } while (popNode &&
               !headNext.compare_exchange_strong(
                   popNode, popNode->next.load(std::memory_order::acquire),
                   std::memory_order::acquire, std::memory_order::relaxed));
    }

    // Data should be no throw move constructible and std::optional
    // requires no heap allocation so this should be safe
    std::optional<T> res{popNode ? std::move(popNode->data) : std::nullopt};

    if (popNode) {
      // In the case where the queue becomes empty and we just stole the last
      // element, we must update tail to point back at the dummy
      Node* expectedTail{popNode};
      Node* dummy{head_.load(std::memory_order::relaxed)};
      tail_.compare_exchange_strong(expectedTail, dummy,
                                    std::memory_order::release,
                                    std::memory_order::relaxed);

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
