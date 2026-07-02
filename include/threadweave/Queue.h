#ifndef TW_QUEUE_H
#define TW_QUEUE_H

#include <threadweave/Hazard.h>

#include <atomic>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
class Queue {
  // --- Data members
  std::atomic<Node*> head_;
  std::atomic<Node*> tail_;
  std::atomic<Node*> toBeDeleted_{nullptr};

 public:
  // --- Ctor, dtor, and assignment operators

  // Ctor (T has a default constructor)
  Queue()
    requires(std::is_default_constructible_v<T>)
      : head_{new Node{.data = T{}, .next = nullptr}},
        tail_{head_.load(std::memory_order::relaxed)} {}

  // Ctor (T has no default constructor)
  template <typename... Args>
  explicit Queue(Args&&... args)
    requires(!std::is_default_constructible_v<T>)
      // Initialize head and tail with dummy node (user is in-charge of
      // constructing a suitable object for the dummy node)
      : head_{
            new Node{.data = T{std::forward<Args>(args)...}, .next = nullptr}},
        tail_{head_.load(std::memory_order::relaxed)} {}

  // Dtor
  ~Queue() {
    // To be executed in single-threaded context, relaxed is sufficient
    Node::deleteNodes(head_, std::memory_order::relaxed);
    Node::deleteNodes(toBeDeleted_, std::memory_order::relaxed);
  }

  // Prevent copy and move operations
  Queue(const Queue&) = delete;
  Queue(Queue&&) = delete;
  Queue& operator=(const Queue&) = delete;
  Queue& operator=(Queue&&) = delete;

  // --- Member functions

  // Add data to beginning of queue
  void push(T data) {
    // Construct new node to store data
    Node* newNode{new Node{.data = std::move(data), .next = nullptr}};

    while (true) {
      // Use an RAII guard for clearing hazard pointer when node is no longer in
      // use
      Internal::HazardGuard hzrdGuard{};

      // Continually acquire tail. At some point, it's next member should be
      // NULL in which case we can finally attach the new node
      Node* tailPtr{hzrdGuard.acquirePointerWithHazard(tail_)};
      std::atomic<Node*>& nextAtomic{tailPtr->next};

      // Try moving the tail pointer chain along so that we can eventually
      // attach our new node
      if (Node* nextPtr{nextAtomic.load(std::memory_order::acquire)};
          nextPtr != nullptr) {
        tail_.compare_exchange_strong(tailPtr, nextPtr,
                                      std::memory_order::release,
                                      std::memory_order::relaxed);
        continue;
      }

      // If our tail's next pointer now points to null, we can finally attach it
      // to the end of our list and then try moving the chain along
      if (nextAtomic.compare_exchange_strong(nullptr, newNode,
                                             std::memory_order::release,
                                             std::memory_order::relaxed)) {
        // Try updating tail to move further down the chain (failures will get
        // resolved by later operations that push further down the chain)
        tail_.compare_exchange_weak(tailPtr, newNode,
                                    std::memory_order::release,
                                    std::memory_order::relaxed);
        return;
      }
    }
  }

  // Get and pop data from our queue
  std::optional<T> pop() {
    // Pointer to the node being popped
    Node* popNode{nullptr};

    {
      // Head is dummy node so acquiring next is always safe
      std::atomic<Node*>& headNext{
          head_.load(std::memory_order::acquire)->next};

      // Use an RAII guard for clearing hazard pointer when node is no longer in
      // use
      Internal::HazardGuard hzrdGuard{};

      do {
        // Acquire pointer to node on top of the stack with thread's hazard
        // pointer pointing to it so that no other threads delete while acessing
        popNode = hzrdGuard.acquirePointerWithHazard(headNext);
      } while (popNode &&
               !headNext.compare_exchange_strong(popNode, popNode->next,
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
        saveForLater(popNode);
      }

      // Try deleting any nodes we previously saved for later
      deleteSavedNodes();
    }

    return res;
  }
};

}  // namespace ThreadWeave

#endif
