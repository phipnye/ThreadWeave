#ifndef TW_QUEUE_H
#define TW_QUEUE_H

#include <threadweave/Constants.h>
#include <threadweave/Hazard.h>
#include <threadweave/Node.h>
#include <threadweave/RetireList.h>

#include <atomic>
#include <optional>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

/**
 * An implementation of a lock-free (Michael-Scott) queue data structure
 * @tparam T type of the object to store in the queue
 */
template <typename T>
  requires(std::is_nothrow_move_constructible_v<T> &&
           std::is_nothrow_move_assignable_v<T>)
class Queue {
  // --- Data members
  using Node = Internal::QueueNode<std::optional<T>>;
  alignas(Internal::CacheLineSize) std::atomic<Node*> head_;
  alignas(Internal::CacheLineSize) std::atomic<Node*> tail_;
  alignas(Internal::CacheLineSize) Internal::RetireList<Node> toBeDeleted_{};

 public:
  // --- Ctor, dtor, and assignment operators

  /**
   * Default construct a Michael-Scott queue
   */
  Queue()
      : head_{new Node{.data = std::nullopt}},  // dummy
        tail_{head_.load(std::memory_order::relaxed)} {}

  /**
   * Free memory associated with the underlying linked list
   */
  ~Queue();

  // Prevent copy and move operations
  Queue(const Queue&) = delete;
  Queue(Queue&&) = delete;
  Queue& operator=(const Queue&) = delete;
  Queue& operator=(Queue&&) = delete;

  // --- Member functions

  /**
   * Add data to back of queue
   * @param data data to store in the queue
   */
  void push(T data);

  /**
   * Pop and return data from the front of the queue
   * @return data from the front of the queue or std::nullopt if queue is empty
   */
  std::optional<T> pop();

  /**
   * Check if the queue is empty.
   * @return true if the queue is empty and false otherwise
   */
  bool empty() const;
};

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T> &&
           std::is_nothrow_move_assignable_v<T>)
Queue<T>::~Queue() {
  // Note that toBeDeleted_ cleans up after itself
  Internal::deleteNodes(head_.load(std::memory_order::relaxed));
}

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T> &&
           std::is_nothrow_move_assignable_v<T>)
void Queue<T>::push(T data) {
  // Construct new node to store data
  Node* pushNode{new Node{.data = std::move(data)}};

  while (true) {
    // Use an RAII guard for clearing hazard pointer when held tail pointer is
    // no longer in use
    const Internal::HazardGuard<0> tailGuard{};
    Node* tailPtr{tailGuard.acquirePointerWithHazard(tail_)};
    std::atomic<Node*>& nextAtomic{tailPtr->next};
    Node* nextPtr{nextAtomic.load(std::memory_order::acquire)};

    // Tail is lagging behind, try moving the tail pointer chain along so that
    // we can eventually attach our new node
    if (nextPtr != nullptr) {
      tail_.compare_exchange_strong(tailPtr, nextPtr,
                                    std::memory_order::release,
                                    std::memory_order::relaxed);
      continue;
    }

    // If our tail's next pointer now points to null, we can finally try
    // attaching it to the end of our list and then try moving the chain along
    if (nextAtomic.compare_exchange_strong(nextPtr, pushNode,
                                           std::memory_order::release,
                                           std::memory_order::relaxed)) {
      // Try updating tail to move further down the chain (failures will get
      // resolved by later operations that push further down the chain)
      tail_.compare_exchange_weak(tailPtr, pushNode, std::memory_order::release,
                                  std::memory_order::relaxed);
      return;
    }
  }
}

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T> &&
           std::is_nothrow_move_assignable_v<T>)
std::optional<T> Queue<T>::pop() {
  // Pointer to the popped node
  Node* oldDummy{nullptr};

  while (true) {
    // Acquire head and head->next pointers with hazard pointers indicating
    // use
    const Internal::HazardGuard<0> headGuard{};
    const Internal::HazardGuard<1> nextGuard{};
    Node* headPtr{headGuard.acquirePointerWithHazard(head_)};
    Node* nextPtr{nextGuard.acquirePointerWithHazard(headPtr->next)};

    // Make sure headPtr is still aligned with head_ atomic
    if (headPtr != head_.load(std::memory_order::acquire)) {
      continue;
    }

    // Empty queue
    if (nextPtr == nullptr) {
      return std::nullopt;
    }

    // If the head and tail point to the same node, the tail is lagging and
    // needs to be pushed along
    if (tail_.load(std::memory_order::acquire) == headPtr) {
      tail_.compare_exchange_strong(headPtr, nextPtr,
                                    std::memory_order::release,
                                    std::memory_order::relaxed);
      continue;
    }

    // Try exchaning the current head with next. If successful, we've popped
    // the node and can return its data
    if (head_.compare_exchange_strong(headPtr, nextPtr,
                                      std::memory_order::acquire,
                                      std::memory_order::relaxed)) {
      // Store the old dummy node so we can retire it and steal the data from
      // the new dummy for our return value
      oldDummy = headPtr;
      oldDummy->data = std::move(nextPtr->data);  // T is no throw move assign
      break;
    }
  }

  // Data should be no throw move constructible and std::optional requires no
  // heap allocation so this should be safe. Note that it is safe to
  // dereference oldDummy at this point. Only the current thread could've
  // gotten out of the CAS loop with oldDummy unlinked from the list. Thus,
  // while other threads may have a pointer pointing to same memory location,
  // they are stuck in the CAS loop and only this thread can delete it or add
  // it to the retire list which happens after this operation.
  std::optional<T> res{std::move(oldDummy->data)};

  // Save the removed node for later if other threads are using it, otherwise
  // delete it immediately
  if (Internal::anyThreadsUsingNode(oldDummy)) {
    toBeDeleted_.saveForLater(oldDummy);
  } else {
    delete oldDummy;
  }

  // Try deleting any nodes we previously saved for later
  toBeDeleted_.deleteNodesChecked();
  return res;
}

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T> &&
           std::is_nothrow_move_assignable_v<T>)
bool Queue<T>::empty() const {
  const Internal::HazardGuard<0> headGuard{};
  Node* headPtr{headGuard.acquirePointerWithHazard(head_)};
  return headPtr->next.load(std::memory_order::relaxed) == nullptr;
}

}  // namespace ThreadWeave

#endif
