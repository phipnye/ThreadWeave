#ifndef TW_NODE_ALLOCATOR_H
#define TW_NODE_ALLOCATOR_H

#include <threadweave/Hazard.h>
#include <threadweave/Node.h>
#include <threadweave/utils.h>

#include <atomic>
#include <memory>

namespace ThreadWeave::Internal {

/**
 * Class for retrieving and recycling nodes for a linked list implementation
 * @tparam Node a linked list node type.
 * @tparam NodesPerBlock number of nodes to allocate at a time (larger values
 * result in fewer heap allocations but could be more wasteful of memory)
 */
template <AllocatorEligibleNode Node, Index NodesPerBlock = 256>
class NodeAllocator {
  // Take head and see which nodes we can steal from the saved list and move to
  // the free list
  static void tryRecycle(Node* saved, Node*& holdFree,
                         Node*& holdSave) noexcept;

  // --- Global cache of nodes to share across threads
  class GlobalNodeCaches {
    // Private helper to allocate a contiguous block of nodes and push to free
    // list
    Node* allocateBlock();

   public:
    std::atomic<Node*> freeHead_{nullptr};  // free nodes
    std::atomic<Node*> saveHead_{nullptr};  // nodes that can't be reused yet

    // Pre-allocate a fixed size of nodes
    GlobalNodeCaches();

    // Free all of the cached memory in global caches
    ~GlobalNodeCaches() noexcept;

    // Prevent copying or moving
    GlobalNodeCaches(const GlobalNodeCaches&) = delete;
    GlobalNodeCaches(GlobalNodeCaches&&) = delete;
    GlobalNodeCaches& operator=(const GlobalNodeCaches&) = delete;
    GlobalNodeCaches& operator=(GlobalNodeCaches&&) = delete;

    // Ask global pool for a free node (falls back to a heap allocation if
    // nothing is available)
    Node* askForNode();

    // Save batch of nodes for later
    void pushSave(Node* batchHead) {
      pushBatch(saveHead_, batchHead);
    }

    // Free batch of nodes to free list
    void pushFree(Node* batchHead) {
      pushBatch(freeHead_, batchHead);
    }

   private:
    // Push a batch of nodes to the head of a global cache
    static void pushBatch(std::atomic<Node*>& cacheHead, Node* batchHead) {
      if (!batchHead) {
        return;
      }

      // Retrieve tail of the batch
      Node* batchTail{batchHead};

      while (batchTail->_internal.next) {
        batchTail = batchTail->_internal.next;
      }

      batchTail->_internal.next = cacheHead.load(MemoryOrder::relaxed);
      while (!cacheHead.compare_exchange_weak(batchTail->_internal.next,
                                              batchHead, MemoryOrder::release,
                                              MemoryOrder::relaxed));
    }
  };

  // Retrieve pointer to the GlobalNodeCaches singleton
  static std::shared_ptr<GlobalNodeCaches> getGlobalCaches();

  // --- Thread-local cache of nodes
  class ThreadNodeCache {
   public:
    std::shared_ptr<GlobalNodeCaches> globalCache_{getGlobalCaches()};
    Node* freeHead_{nullptr};  // free nodes
    Node* saveHead_{nullptr};  // nodes that can't be reused yet

    ThreadNodeCache() = default;

    // Push everything to one of the global pools of nodes
    ~ThreadNodeCache();

    // Prevent copying or moving
    ThreadNodeCache(const ThreadNodeCache&) = delete;
    ThreadNodeCache(ThreadNodeCache&&) = delete;
    ThreadNodeCache& operator=(const ThreadNodeCache&) = delete;
    ThreadNodeCache& operator=(ThreadNodeCache&&) = delete;

    // Ask global cache for a free node
    Node* askGlobalForNode() {
      return globalCache_->askForNode();
    }
  };

  static ThreadNodeCache& getThreadCaches();

 public:
  // --- Ctors, dtor, and assignment operators
  NodeAllocator() = delete;
  ~NodeAllocator() = delete;
  NodeAllocator(const NodeAllocator&) = delete;
  NodeAllocator(NodeAllocator&&) = delete;
  NodeAllocator& operator=(const NodeAllocator&) = delete;
  NodeAllocator& operator=(NodeAllocator&&) = delete;

  // --- Member functions

  /**
   * Retrieve an allocation for an available node
   * @return a pointer to an available dynamically allocated node
   */
  static Node* allocate();

  /**
   * Retire a node by either recycling it to the free list or saving it for
   * later if other threads are using it
   * @param node pointer to a singly linked list node to recycle once all
   * other threads are done using it
   */
  static void deallocate(Node* node) noexcept;
};

template <AllocatorEligibleNode Node, Index NodesPerBlock>
void NodeAllocator<Node, NodesPerBlock>::tryRecycle(Node* saved,
                                                    Node*& holdFree,
                                                    Node*& holdSave) noexcept {
  while (saved) {
    Node* curr{saved};
    saved = saved->_internal.next;

    if (!Internal::anyThreadsUsingNode(curr)) {
      curr->reset();
      curr->_internal.next = holdFree;
      holdFree = curr;
    } else {
      curr->_internal.next = holdSave;
      holdSave = curr;
    }
  }
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches::allocateBlock() {
  // Allocate a full block of nodes (we do this to minimize the number of
  // times malloc has to be called)
  Node* block{new Node[NodesPerBlock]};

  // Reset performs a "true" value initialization of the nodes
  for (Index i{0}; i < NodesPerBlock; ++i) {
    block[i].reset();
  }

  // Mark the absolute start of this OS allocation chunk so we know which
  // nodes to free in the destructor
  block[0]._internal.isBlockStart = true;

  // Chain the remaining nodes together and ensure their flags are false
  for (Index i{1}; i + 1 < NodesPerBlock; ++i) {
    block[i]._internal.next = &block[i + 1];
  }

  // Retrieve the heads and tails of the now chained block (nodes 1-255 will
  // be pushed to the free list while the block head will be returned to the
  // calling thread)
  static_assert(NodesPerBlock > 1,
                "Must allocate more than one node at a time for proper "
                "chaining logic");
  Node* batchHead{&block[1]};
  Node* batchTail{&block[NodesPerBlock - 1]};

  // Push this chain onto the free list
  batchTail->_internal.next = freeHead_.load(MemoryOrder::relaxed);
  while (!freeHead_.compare_exchange_weak(batchTail->_internal.next, batchHead,
                                          MemoryOrder::release,
                                          MemoryOrder::relaxed)) {}

  // Hand the stolen first node directly back to the calling thread
  return &block[0];
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches::GlobalNodeCaches() {
  // Preallocate a block of the set number of nodes
  Node* initialNode{allocateBlock()};

  // Chain the "stolen" first node back onto the front of the free list
  initialNode->_internal.next = freeHead_.load(MemoryOrder::relaxed);
  freeHead_.store(initialNode, MemoryOrder::relaxed);
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node,
              NodesPerBlock>::GlobalNodeCaches::~GlobalNodeCaches() noexcept {
  // Note, this destructor is completely safe at the time it gets called by the
  // use of shared pointers with reference counting. So long as the a thread
  // cache holds a reference, this destructor won't get called. This prevents
  // free any potential use-after-free issues or cases where the thread cache
  // destructors push to the global cache after the global's destructor

  // Keep track of the nodes that are block starts so we can free memory
  // safely
  Node* blockStarts{nullptr};

  // Gather all of the nodes that are the starts of a block across all of
  // our lists
  for (std::atomic<Node*>* atomicHead : {&freeHead_, &saveHead_}) {
    Node* head{atomicHead->load(MemoryOrder::relaxed)};

    while (head) {
      Node* const curr{head};
      head = head->_internal.next;

      if (curr->_internal.isBlockStart) {
        curr->_internal.next = blockStarts;
        blockStarts = curr;
      }
    }
  }

  // Finally call delete on all of the block starts
  while (blockStarts) {
    // ReSharper disable once CppLocalVariableMayBeConst
    Node* const curr{blockStarts};
    blockStarts = blockStarts->_internal.next;
    delete[] curr;
  }
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches::askForNode() {
  Node* freeNode{nullptr};

  {
    // Use a hazard guard to acquire the head of the free list and try to pop
    // it, this prevents an ABA problem by ensuring the head gets retired to the
    // save list (thus failing the CAS) if threads in this loop are trying to
    // pop it
    HazardGuard<HazardSlot::Alloc2> guard{};

    while (true) {
      freeNode = guard.acquirePointerWithHazard(freeHead_);

      // No more nodes to acquire
      if (!freeNode) {
        break;
      }

      // Try popping the head of the free list (note this is safe from ABA)
      // because other threads that may have already popped and tried to
      // deallocate this node will have pushed it back to saveHead and thus fail
      // this CAS
      if (freeHead_.compare_exchange_strong(freeNode, freeNode->_internal.next,
                                            MemoryOrder::acquire,
                                            MemoryOrder::relaxed)) {
        break;
      }
    }
  }

  // If we successfully detached a free node from the freelist
  if (freeNode) {
    return freeNode;
  }

  // Try recycling nodes from the save list
  // clang-format off
  if (Node* saved{saveHead_.exchange(nullptr, MemoryOrder::acquire)}) {
    // clang-format on
    Node* holdFree{nullptr};  // Nodes that can be moved to free list
    Node* holdSave{nullptr};  // Nodes that remain in 'saved' state

    // Use the consolidated helper to sort the stolen global nodes
    tryRecycle(saved, holdFree, holdSave);

    // Return the still-pinned nodes back to the global save pool
    pushSave(holdSave);

    // If we uncovered safe nodes, peel one off to return, cache the rest
    if (holdFree) {
      freeNode = holdFree;
      holdFree = holdFree->_internal.next;
      freeNode->_internal.next = nullptr;
      pushFree(holdFree);
      return freeNode;
    }
  }

  // Fallback to OS allocation (this should happen rarely since we allocate
  // blocks of nodes at a time)
  return allocateBlock();
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
std::shared_ptr<typename NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches>
NodeAllocator<Node, NodesPerBlock>::getGlobalCaches() {
  static auto global{std::make_shared<GlobalNodeCaches>()};
  return global;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::ThreadNodeCache::~ThreadNodeCache() {
  globalCache_->pushFree(freeHead_);
  Node* holdFree{nullptr};  // nodes that are now free
  Node* holdSave{nullptr};  // nodes that need to remain saved for later
  tryRecycle(saveHead_, holdFree, holdSave);
  globalCache_->pushFree(holdFree);
  globalCache_->pushSave(holdSave);
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::ThreadNodeCache&
NodeAllocator<Node, NodesPerBlock>::getThreadCaches() {
  thread_local ThreadNodeCache cache{};
  return cache;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<Node, NodesPerBlock>::allocate() {
  ThreadNodeCache& local{getThreadCaches()};

  // Try recycling local nodes if necessary
  if (!local.freeHead_) {
    // Isolate the current local saved chain
    Node* saved{local.saveHead_};
    local.saveHead_ = nullptr;
    tryRecycle(saved, local.freeHead_, local.saveHead_);
  }

  // Return node
  Node* node{nullptr};

  // If we have a free node available use it
  if (local.freeHead_) {
    node = local.freeHead_;
    local.freeHead_ = node->_internal.next;
  } else {
    // Ask global pool for a node (if it has a free node, it will provide it, if
    // not this will perform a heap allocation which is not lock-free though
    // this fallback should occur infrequently
    node = local.askGlobalForNode();
  }

  return node;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
void NodeAllocator<Node, NodesPerBlock>::deallocate(Node* node) noexcept {
  if (!node) {
    return;
  }

  ThreadNodeCache& local{getThreadCaches()};

  // If no threads are using node, we can immediately put it back on the free
  // list, otherwise we have to save it for later
  if (!Internal::anyThreadsUsingNode(node)) {
    node->reset();
    node->_internal.next = local.freeHead_;
    local.freeHead_ = node;
  } else {
    node->_internal.next = local.saveHead_;
    local.saveHead_ = node;
  }
}

}  // namespace ThreadWeave::Internal

#endif
