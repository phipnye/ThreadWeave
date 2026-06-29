#ifndef TW_STACK_H
#define TW_STACK_H

#include <threadweave/hazard.h>

#include <atomic>
#include <optional>
#include <type_traits>
#include <utility>

namespace ThreadWeave {

// A thread-safe lock-free stack data structure
template <typename T>
  requires(std::is_nothrow_move_constructible_v<T>)
class Stack {
  struct Node {
    T data_;
    Node* next_;
  };

  // --- Data members
  std::atomic<Node*> head_{nullptr};
  std::atomic<Node*> toBeDeleted_{nullptr};

 public:
  // --- Ctor and assignment

  // Default ctor
  Stack() = default;

  // Dtor
  ~Stack() {
    deleteNodes(head_.load());
    deleteNodes(toBeDeleted_.load());
  }

  // Prevent copy and move operations
  Stack(const Stack&) = delete;
  Stack(Stack&&) = delete;
  Stack& operator=(const Stack&) = delete;
  Stack& operator=(Stack&&) = delete;

  // --- Member functions

  // Push data onto the stack
  void push(T data) {
    // Generate new node
    Node* newNode{new Node{std::move(data), head_.load()}};

    // Continually try to assign new node as the head
    while (!head_.compare_exchange_weak(newNode->next_, newNode));
  }

  // Pop data from the top of the stack
  std::optional<T> pop() {
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

    // Clear the hazard pointer so the node can be deleted if it's added to the
    // list of nodes that need to be deleted later
    hp.store(nullptr);

    // Return null if the stack is empty or the data of the popped node
    // otherwise (data should be no throw move constructible and std::optional
    // requires no heap allocation so this should be safe)
    std::optional<T> res{oldHead ? std::make_optional(std::move(oldHead->data_))
                                 : std::nullopt};

    if (oldHead) {
      // Save the popped node for later if other threads are using it, otherwise
      // delete it immediately
      if (!Internal::anyThreadsUsingNode(oldHead)) {
        delete oldHead;
      } else {
        saveForLater(oldHead);
      }

      // Try deleting any nodes we previously saved for later
      deleteSavedNodes();
    }

    return res;
  }

 private:
  // Add the passed node to the beginning of our list of nodes to be deleted
  // later
  void saveForLater(Node* const newNode) {
    // Prepend to beginning of list
    newNode->next_ = toBeDeleted_.load();
    while (!toBeDeleted_.compare_exchange_weak(newNode->next_, newNode));
  }

  // Try freeing memory of nodes that had to be saved for later due to use by
  // other threads
  void deleteSavedNodes() {
    // Acquire the list of nodes that still need deleting
    Node* list{toBeDeleted_.exchange(nullptr)};

    // Store nodes that we still need to save for later
    Node* saveHead{nullptr};
    Node* saveTail{nullptr};

    while (list) {
      // Look at the current list of nodes to be deleted
      Node* curr{list};
      list = list->next_;
      curr->next_ = nullptr;

      // If no threads are using curr, delete it, otherwise add back to the list
      // of nodes to be deleted
      if (!Internal::anyThreadsUsingNode(curr)) {
        delete curr;
      } else {
        if (saveTail) {
          saveTail->next_ = curr;
          saveTail = saveTail->next_;
        } else {
          saveHead = curr;
          saveTail = curr;
        }
      }
    }

    // Add our saved list back to the list of nodes to be deleted later
    if (saveTail) {
      saveTail->next_ = toBeDeleted_.load();
      while (!toBeDeleted_.compare_exchange_weak(saveTail->next_, saveHead));
    }
  }

  // Delete a list of nodes (mainly for dtor)
  static void deleteNodes(const Node* list) {
    while (list) {
      const Node* const curr{list};
      list = list->next_;
      delete curr;
    }
  }
};

}  // namespace ThreadWeave

#endif
