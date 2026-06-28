#ifndef TW_STACK_H
#define TW_STACK_H

#include <threadweave/hazard.h>

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
    Node* next_;

    // Ctor
    explicit Node(T data, Node* next = nullptr)
        : data_{std::make_shared<T>(std::move(data))}, next_{std::move(next)} {}
  };

  // --- Data members
  std::atomic<Node*> head_{nullptr};

 public:
  // --- Ctor and assignment

  // Default ctor
  Stack() = default;

  // Prevent copy and move operations
  Stack(const Stack&) = delete;
  Stack(Stack&&) = delete;
  Stack& operator=(const Stack&) = delete;
  Stack& operator=(Stack&&) = delete;

  // Push data onto the stack
  void push(T data) {
    // Generate new node
    Node* newNode{new Node{std::move(data), head_.load()}};

    // Continually try to assign new node as the head
    while (!head_.compare_exchange_weak(newNode->next_, newNode));
  }

  // Pop data from the top of the stack
  std::shared_ptr<T> pop() {
    // Get the hazard pointer for the current thread
    std::atomic<void*>& hp{Internal::getThreadHazardPointer()};

    // Retrieve node at top of stack
    Node* oldHead{head_.load()};

    // While old head is non-null, continually try taking it from the top of the
    // stack
    do {
      Node* tmp{nullptr};

      // Continually try to acquire the head of our list and store it with our
      // hazard pointer
      do {
        tmp = oldHead;
        hp.store(oldHead);
        oldHead = head_.load();
      } while (oldHead != tmp);

    } while (oldHead &&
             !head_.compare_exchange_strong(oldHead, oldHead->next_));

    //

    // Return null if the stack is empty or the data of the popped node
    // otherwise
    std::shared_ptr<T> res{nullptr};

    if (oldHead) {
      std::swap(res, oldHead->data_);

      if (!Internal::anyThreadsUsingNode(oldHead)) {
        saveForLater(oldHead);
      } else {
        delete oldHead;
      }

      deleteSavedNodes();
    }

    return res;
  }
};

}  // namespace ThreadWeave

#endif
