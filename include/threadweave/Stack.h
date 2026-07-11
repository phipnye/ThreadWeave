#ifndef TW_STACK_H
#define TW_STACK_H

#include <threadweave/Hazard.h>
#include <threadweave/Node.h>
#include <threadweave/NodeAllocator.h>
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
  using Allocator = Internal::NodeAllocator<Node>;
  alignas(Internal::CacheLineSize) std::atomic<Node*> head_{nullptr};

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
  bool empty() const noexcept;
};

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
Stack<T>::~Stack() {
  Node* head{head_.load(std::memory_order::relaxed)};

  while (head) {
    // ReSharper disable once CppLocalVariableMayBeConst
    Node* const curr{head};
    head = head->next;

    // Explicitly destroy the user's data payload
    curr->data.~T();

    // Return the node memory back to the allocator pools safely
    Allocator::deallocate(curr);
  }
}

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
void Stack<T>::push(T data) {
  // Grab raw, recycled node memory from the block allocator (may very rarely
  // perform a heap allocation which is not lock-free)
  Node* newNode{Allocator::allocate()};

  // Move data into the node
  new (static_cast<void*>(&newNode->data)) T{std::move(data)};

  // Push new node onto the stack
  newNode->next = head_.load(std::memory_order::relaxed);
  while (!head_.compare_exchange_weak(newNode->next, newNode,
                                      std::memory_order::release,
                                      std::memory_order::relaxed)) {}
}

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
std::optional<T> Stack<T>::pop() {
  // Pointer to node on top of the stack
  Node* popNode{nullptr};

  {
    // Use an RAII guard for clearing hazard pointer when node is no longer in
    // use
    Internal::HazardGuard<0> headGuard{};

    do {
      // Acquire pointer pointing to head node with a hazard pointer indicating
      // current thread's use
      popNode = headGuard.acquirePointerWithHazard(head_);
    } while (popNode && !head_.compare_exchange_strong(
                            popNode, popNode->next, std::memory_order::acquire,
                            std::memory_order::relaxed));
  }

  // Empty stack
  if (!popNode) {
    return std::nullopt;
  }

  // Data should be no throw move constructible and std::optional requires no
  // heap allocation so this should be safe. Note that it is safe to
  // dereference popNode at this point. Only the current thread could've
  // gotten out of the CAS loop with popNode unlinked from the list. Thus,
  // while other threads may have a pointer pointing to same memory location,
  // they are stuck in the CAS loop and only this thread can delete it or add
  // it to the retire list which happens after this operation.
  std::optional<T> res{std::move(popNode->data)};

  // Explicitly call the destructor on the remaining moved-from data state.
  // skipping this could cause memory leaks later in the program when node
  // memory gets recycled and overwritten
  popNode->data.~T();

  // Hand the node back to the allocator. It automatically checks hazard
  // pointers and places it in either the local free list or local save list.
  Allocator::deallocate(popNode);
  return res;
}

template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
bool Stack<T>::empty() const noexcept {
  return head_.load(std::memory_order::relaxed) == nullptr;
}

}  // namespace ThreadWeave

#endif
