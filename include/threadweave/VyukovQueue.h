#ifndef TW_VYUKOV_QUEUE_H
#define TW_VYUKOV_QUEUE_H

#include <threadweave/Node.h>
#include <threadweave/NodeAllocator.h>
#include <threadweave/utils.h>

#include <atomic>
#include <optional>
#include <type_traits>

namespace ThreadWeave {

template <typename T>
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
class VyukovQueue {
  using Node = Internal::QueueNode<std::optional<T>>;
  using Allocator = Internal::NodeAllocator<Node>;

  // --- Data members
  alignas(Internal::CacheLineSize) std::atomic<Node*> tail_{nullptr};
  alignas(Internal::CacheLineSize) Node* head_{nullptr};

 public:
  // --- Ctors, dtor, and assignment operators

  /**
   * Default construct a VyukovQueue
   */
  VyukovQueue();

  /**
   * Destruct a VyukovQueue instance
   */
  ~VyukovQueue() noexcept;

  // Prevent copying and moving
  VyukovQueue(const VyukovQueue&) = delete;
  VyukovQueue(VyukovQueue&&) = delete;
  VyukovQueue& operator=(const VyukovQueue&) = delete;
  VyukovQueue& operator=(VyukovQueue&&) = delete;

  /**
   * Add data to back of queue
   * @param data data to store in the queue
   */
  void push(T data);

  /**
   * Pop and return data from the front of the queue
   * @return data from the front of the queue or std::nullopt if queue is empty
   */
  std::optional<T> pop() noexcept;

  // /**
  //  * Check if the queue is empty.
  //  * @return true if the queue is empty and false otherwise
  //  */
  // bool empty() const;
};

template <typename T>
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
VyukovQueue<T>::VyukovQueue() {
  // Allocate a dummy node and point the head and tail at the dummy
  Node* dummy{Allocator::allocate()};
  tail_.store(dummy, MemoryOrder::relaxed);
  head_ = dummy;
}

template <typename T>
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
VyukovQueue<T>::~VyukovQueue() noexcept {
  while (head_) {
    Node* const curr{head_};
    head_ = head_->next.load(MemoryOrder::relaxed);
    Allocator::deallocate(curr);
  }
}

template <typename T>
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
void VyukovQueue<T>::push(T data) {
  // Construct new node to store data
  Node* pushNode{Allocator::allocate()};
  pushNode->data = std::move(data);

  // Append the new node to the prior tail
  Node* oldTail{tail_.exchange(pushNode, MemoryOrder::acq_rel)};
  oldTail->next.store(pushNode, MemoryOrder::release);
}

template <typename T>
  requires(std::is_nothrow_default_constructible_v<T> &&
           std::is_nothrow_move_constructible_v<T>)
std::optional<T> VyukovQueue<T>::pop() noexcept {
  // Non-empty queue
  if (Node* const next{head_->next.load(MemoryOrder::acquire)}) {
    // Release node of the prior head
    Allocator::deallocate(head_);

    // Move head to next and then steal the data
    head_ = next;
    return std::move(next->data);
  }

  // Empty queue
  return std::nullopt;
}

// template <typename T>
//   requires(std::is_nothrow_default_constructible_v<T> &&
//            std::is_nothrow_move_constructible_v<T>)
// bool VyukovQueue<T>::empty() const {
//   return head_ == tail_.load(MemoryOrder::relaxed);
// }

}  // namespace ThreadWeave

#endif
