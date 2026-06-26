#ifndef TW_STACK_H
#define TW_STACK_H

#include <atomic>
#include <memory>
#include <utility>

namespace ThreadWeave {

// A thread-safe lock-free stack data structure
template <typename T>
class Stack {
  // --- Node helper class
  struct Node {
    // --- Data members
    std::shared_ptr<T> data_;
    std::shared_ptr<Node> next_;

    // Ctor
    explicit Node(T data, std::shared_ptr<Node> next = nullptr)
        : data_{std::make_shared<T>(std::move(data))}, next_{std::move(next)} {}
  };

  // --- Data members
  std::atomic<std::shared_ptr<Node>> head_{nullptr};

 public:
  // --- Ctor and assignment

  // Default ctor
  Stack() = default;

  // Prevent copy operations
  Stack(const Stack&) = delete;
  Stack& operator=(const Stack&) = delete;

  // Push data onto the stack
  void push(T data) {
    // Generate new node
    auto newNode{std::make_shared<Node>(std::move(data), head_.load())};

    // Continually try to assign new node as the head
    while (!head_.compare_exchange_weak(newNode->next_, newNode));
  }

  // Pop data from the top of the stack
  std::shared_ptr<T> pop() {
    // Retrieve node at top of stack
    auto oldHead{head_.load()};

    // While old head is non-null, continually try taking it from the top of the
    // stack
    while (oldHead && !head_.compare_exchange_weak(oldHead, oldHead->next_));

    // Return null if the stack is empty
    return oldHead ? oldHead->data_ : nullptr;
  }
};

}  // namespace ThreadWeave

#endif
