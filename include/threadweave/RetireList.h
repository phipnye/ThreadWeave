#ifndef TW_RETIRE_LIST_H
#define TW_RETIRE_LIST_H

#include <threadweave/Hazard.h>
#include <threadweave/Node.h>

#include <atomic>

namespace ThreadWeave::Internal {

// Class for storing list of nodes that need to be freed later due to use by
// other threads
template <typename Node>
  requires(HasRawNextPointer<Node>)
class RetireList {
  // --- Data members

  // We have to wrap the nodes of a linked list with another node to prevent
  // data races between CAS loop for pop operations and appending them to our
  // retire list
  using NodeWrapper = SinglyLinkedListNode<Node*>;

  // List of nodes to be deleted later in the program
  std::atomic<NodeWrapper*> tbdList_{nullptr};

 public:
  // --- Ctors, dtor, and assignment operators

  // Default ctor
  RetireList() = default;

  // Dtor
  ~RetireList() {
    NodeWrapper* deleteList{tbdList_.load(std::memory_order::relaxed)};

    while (deleteList) {
      NodeWrapper* curr{deleteList};
      deleteList = deleteList->next;
      delete curr->data;  // Must delete underlying node
      delete curr;        // And the node wrapping the node
    }
  }

  // Prevent copy and move operations
  RetireList(const RetireList&) = delete;
  RetireList(RetireList&&) = delete;
  RetireList& operator=(const RetireList&) = delete;
  RetireList& operator=(RetireList&&) = delete;

  // --- Member functions

  // Save node for later since it cannot be freed yet due to use by other nodes
  void saveForLater(Node* node) {
    // Keep trying to prepend node to front of our list
    NodeWrapper* newNodeWrapper{new NodeWrapper{
        .data = node, .next = tbdList_.load(std::memory_order::relaxed)}};
    while (!tbdList_.compare_exchange_weak(newNodeWrapper->next, newNodeWrapper,
                                           std::memory_order::release,
                                           std::memory_order::relaxed));
  }

  // Delete nodes after checking to make sure they are not being used by other
  // threads
  void deleteNodesChecked() {
    // Switch out "buffers" so other threads can write to member while this
    // thread operates on the "stolen" list of nodes
    NodeWrapper* deleteList{
        tbdList_.exchange(nullptr, std::memory_order::acq_rel)};

    // Store any nodes that we still cannot delete due to use by other threads
    NodeWrapper* saveHead{nullptr};
    NodeWrapper* saveTail{nullptr};

    while (deleteList) {
      NodeWrapper* curr{deleteList};
      deleteList = deleteList->next;
      curr->next = nullptr;

      // Delete right away if no threads are using curr's data, otherwise,
      // append it to our list for saving later
      if (!Internal::anyThreadsUsingNode(curr->data)) {
        delete curr->data;
        delete curr;
      } else {
        if (saveTail) {
          saveTail->next = curr;
          saveTail = saveTail->next;
        } else {
          saveHead = curr;
          saveTail = curr;
        }
      }
    }

    // Save out list of nodes that still couldn't be deleted
    if (saveTail) {
      saveTail->next = tbdList_.load(std::memory_order::relaxed);
      while (!tbdList_.compare_exchange_weak(saveTail->next, saveHead,
                                             std::memory_order::release,
                                             std::memory_order::relaxed));
    }
  }
};

}  // namespace ThreadWeave::Internal

#endif
