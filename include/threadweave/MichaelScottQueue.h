#ifndef TW_MICHAEL_SCOTT_QUEUE_H
#define TW_MICHAEL_SCOTT_QUEUE_H

#include <threadweave/Hazard.h>
#include <threadweave/Node.h>
#include <threadweave/NodeAllocator.h>
#include <threadweave/enums.h>
#include <threadweave/utils.h>

#include <atomic>
#include <optional>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

/**
 * An implementation of a lock-free (Michael-Scott) queue data structure. This
 * queue supports multiple consumers, multiplie producers semantics.
 * @tparam T type of the object to store in the queue
 */
template <typename T>
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
class MichaelScottQueue {
  // --- Data members
  using Node = Internal::QueueNode<std::optional<T>>;
  using Allocator = Internal::NodeAllocator<Node>;
  alignas(Internal::CacheLineSize) std::atomic<Node*> head_{nullptr};
  alignas(Internal::CacheLineSize) std::atomic<Node*> tail_{nullptr};

 public:
  // --- Ctor, dtor, and assignment operators

  /**
   * Default construct a Michael-Scott queue
   */
  MichaelScottQueue();

  /**
   * Free memory associated with the underlying linked list
   */
  ~MichaelScottQueue() noexcept;

  // Prevent copy and move operations
  MichaelScottQueue(const MichaelScottQueue&) = delete;
  MichaelScottQueue(MichaelScottQueue&&) = delete;
  MichaelScottQueue& operator=(const MichaelScottQueue&) = delete;
  MichaelScottQueue& operator=(MichaelScottQueue&&) = delete;

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
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
MichaelScottQueue<T>::MichaelScottQueue() {
  // Acquire a raw node block for the initial dummy node
  Node* dummy{Allocator::allocate()};

  // Point both head and tail to the dummy
  head_.store(dummy, MemoryOrder::relaxed);
  tail_.store(dummy, MemoryOrder::relaxed);
}

template <typename T>
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
MichaelScottQueue<T>::~MichaelScottQueue() noexcept {
  Node* head{head_.load(MemoryOrder::relaxed)};

  while (head) {
    Node* curr{head};
    head = head->next.load(MemoryOrder::relaxed);
    Allocator::deallocate(curr);
  }
}

template <typename T>
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
void MichaelScottQueue<T>::push(T data) {
  // Construct new node to store data
  Node* pushNode{Allocator::allocate()};
  pushNode->data = std::move(data);

  while (true) {
    // Use an RAII guard for clearing hazard pointer when held tail pointer is
    // no longer in use
    const Internal::HazardGuard<Internal::HazardSlot::Queue0> tailGuard{};
    Node* tailPtr{tailGuard.acquirePointerWithHazard(tail_)};
    std::atomic<Node*>& nextAtomic{tailPtr->next};
    Node* nextPtr{nextAtomic.load(MemoryOrder::acquire)};

    // Tail is lagging behind, try moving the tail pointer chain along so that
    // we can eventually attach our new node
    if (nextPtr != nullptr) {
      tail_.compare_exchange_strong(tailPtr, nextPtr, MemoryOrder::release,
                                    MemoryOrder::relaxed);
      continue;
    }

    // If our tail's next pointer now points to null, we can finally try
    // attaching it to the end of our list and then try moving the chain along
    if (nextAtomic.compare_exchange_strong(
            nextPtr, pushNode, MemoryOrder::release, MemoryOrder::relaxed)) {
      // Try updating tail to move further down the chain (failures will get
      // resolved by later operations that push further down the chain)
      tail_.compare_exchange_strong(tailPtr, pushNode, MemoryOrder::release,
                                    MemoryOrder::relaxed);
      return;
    }
  }
}

template <typename T>
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
std::optional<T> MichaelScottQueue<T>::pop() {
  // Pointer to the popped node
  Node* oldDummy{nullptr};

  // Store the result
  std::optional<T> res{std::nullopt};

  while (true) {
    // Acquire head and head->next pointers with hazard pointers indicating
    // use
    const Internal::HazardGuard<Internal::HazardSlot::Queue0> headGuard{};
    const Internal::HazardGuard<Internal::HazardSlot::Queue1> nextGuard{};
    Node* headPtr{headGuard.acquirePointerWithHazard(head_)};
    Node* nextPtr{nextGuard.acquirePointerWithHazard(headPtr->next)};

    // Make sure headPtr is still aligned with head_ atomic
    if (headPtr != head_.load(MemoryOrder::acquire)) {
      continue;
    }

    // Empty queue
    if (nextPtr == nullptr) {
      return std::nullopt;
    }

    // If the head and tail point to the same node, the tail is lagging and
    // needs to be pushed along
    if (tail_.load(MemoryOrder::acquire) == headPtr) {
      tail_.compare_exchange_strong(headPtr, nextPtr, MemoryOrder::release,
                                    MemoryOrder::relaxed);
      continue;
    }

    // Try exchaning the current head with next. If successful, we've popped
    // the node and can return its data
    if (head_.compare_exchange_strong(headPtr, nextPtr, MemoryOrder::acquire,
                                      MemoryOrder::relaxed)) {
      // Steal the data from the next pointer
      res = std::move(nextPtr->data);

      // Store the old dummy node so we can retire it
      oldDummy = headPtr;
      break;
    }
  }

  if (oldDummy) {
    // Return the node back to the allocator to check hazard pointers and
    // recycle
    Allocator::deallocate(oldDummy);
  }

  return res;
}

template <typename T>
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
bool MichaelScottQueue<T>::empty() const {
  const Internal::HazardGuard<Internal::HazardSlot::Queue0> headGuard{};
  Node* headPtr{headGuard.acquirePointerWithHazard(head_)};
  return headPtr->next.load(MemoryOrder::relaxed) == nullptr;
}

}  // namespace ThreadWeave

#endif
