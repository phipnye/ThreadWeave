#ifndef TW_RETIRE_LIST_H
#define TW_RETIRE_LIST_H

#include <threadweave/Constants.h>
#include <threadweave/Hazard.h>
#include <threadweave/Node.h>
#include <threadweave/StackOps.h>

#include <atomic>

namespace ThreadWeave::Internal {

/**
 * Class for storing list of nodes that need to be freed later due to use by
 * other threads
 * @tparam Node a linked list node type
 */
template <typename Node>
  requires(HasRetireNextPointer<Node>)
class RetireList {
  // --- Data members
  std::atomic<Node*> tbdList_{nullptr};

 public:
  // --- Ctors, dtor, and assignment operators

  /**
   * Default construct a retirement list
   */
  RetireList() = default;

  /**
   * Free all memory associated with the to be deleted list
   */
  ~RetireList() {
    Node* delList{tbdList_.load(std::memory_order::relaxed)};

    while (delList) {
      // ReSharper disable once CppLocalVariableMayBeConst
      Node* const curr{delList};
      delList = delList->retireNext;
      delete curr;
    }
  }

  // Prevent copy and move operations
  RetireList(const RetireList&) = delete;
  RetireList(RetireList&&) = delete;
  RetireList& operator=(const RetireList&) = delete;
  RetireList& operator=(RetireList&&) = delete;

  // --- Member functions

  /**
   * Save node for later since it cannot be freed yet due to use by other nodes
   * @param node pointer to a singly linked list node to store into the to be
   * deleted list
   */
  void saveForLater(Node* node) noexcept {
    stackPush<Node, &Node::retireNext>(tbdList_, node);
  }

  /**
   * Delete nodes after checking to make sure they are not being used by other
   * threads
   */
  void deleteNodesChecked() {
    // Switch out "buffers" so other threads can write to member while this
    // thread operates on the "stolen" list of nodes
    Node* delList{tbdList_.exchange(nullptr, std::memory_order::acq_rel)};

    // Store any nodes that we still cannot delete due to use by other threads
    Node* saveHead{nullptr};
    Node* saveTail{nullptr};

    while (delList) {
      Node* curr{delList};
      delList = delList->retireNext;
      curr->retireNext = nullptr;

      // Delete right away if no threads are using curr's data, otherwise,
      // append it to our list for saving later
      if (Internal::anyThreadsUsingNode(curr)) {
        if (saveTail) {
          saveTail->retireNext = curr;
          saveTail = saveTail->retireNext;
        } else {
          saveHead = curr;
          saveTail = curr;
        }
      } else {
        delete curr;
      }
    }

    // Save out list of nodes that still couldn't be deleted
    if (saveTail) {
      saveTail->retireNext = tbdList_.load(std::memory_order::relaxed);
      while (!tbdList_.compare_exchange_weak(saveTail->retireNext, saveHead,
                                             std::memory_order::release,
                                             std::memory_order::relaxed));
    }
  }
};

}  // namespace ThreadWeave::Internal

#endif
