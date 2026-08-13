#ifndef TW_NODE_ALLOCATOR_H
#define TW_NODE_ALLOCATOR_H

#include <threadweave/internal/Hazard.h>
#include <threadweave/internal/Node.h>
#include <threadweave/internal/utils.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

namespace ThreadWeave::Internal {

/**
 * Class for retrieving and recycling nodes for a linked list implementation
 * @tparam Node a linked list node type.
 * @tparam NodesPerBlock number of nodes to allocate at a time (larger values
 * result in fewer heap allocations but could be more wasteful of memory)
 */
template <AllocatorEligibleNode Node, Index NodesPerBlock = 64>
class NodeAllocator {
  /**
   *Take head and see which nodes we can steal from the saved list and move to
   * the free list
   * @return the number of nodes that were preserved in the save list
   */
  static Index tryRecycle(Node* saved, Node*& holdFree, Node*& holdSave,
                          std::vector<const void*>& snapshot) noexcept;

  // --- Global cache of nodes to share across threads
  class GlobalNodeCaches {
    // Private helper to allocate a contiguous block of nodes and push to free
    // list
    Node* allocateBlock();

   public:
#ifndef TW_NDEBUG
    // Keep track of the number of allocations to make sure there are no leaks
    // (NOTE: This member must be initialized BEFORE freeHead_ gets initialized
    // with allocateBlock() which modifies this value)
    alignas(kCacheLineSize) mutable std::atomic<Index> nAllocs_{0};
#endif

    // Free nodes
    alignas(kCacheLineSize) std::atomic<Node*> freeHead_;

    // Nodes that can't be reused yet
    alignas(kCacheLineSize) std::atomic<Node*> saveHead_{nullptr};

    // Pre-allocate a fixed size of nodes
    GlobalNodeCaches();

    // Free all of the cached memory in global caches
    ~GlobalNodeCaches();

    // Prevent copying or moving
    GlobalNodeCaches(const GlobalNodeCaches&) = delete;
    GlobalNodeCaches(GlobalNodeCaches&&) = delete;
    GlobalNodeCaches& operator=(const GlobalNodeCaches&) = delete;
    GlobalNodeCaches& operator=(GlobalNodeCaches&&) = delete;

    // Ask global pool for a free node (falls back to a heap allocation if
    // nothing is available)
    Node* askForNode();

    // Save batch of nodes for later
    void pushSave(Node* batchHead);

    // Free batch of nodes to free list
    void pushFree(Node* batchHead);

   private:
    // Push a batch of nodes to the head of a global cache
    static void pushBatch(std::atomic<Node*>& cacheHead, Node* batchHead);
  };

  // Retrieve pointer to the GlobalNodeCaches singleton
  static std::shared_ptr<GlobalNodeCaches> getGlobalCaches();

  // --- Thread-local cache of nodes
  class ThreadNodeCache {
   public:
    std::shared_ptr<GlobalNodeCaches> globalCache_{getGlobalCaches()};
    Node* freeHead_{nullptr};  // free nodes
    Node* saveHead_{nullptr};  // nodes that can't be reused yet

    // Store info related to deallocation to delay when they're necessary and
    // amortize the cost
    std::vector<const void*> hazardSnapshot_{};
    Index saveCnt_{0};

    // Reserve space for hazard snapshots
    ThreadNodeCache();

    // Push everything to one of the global pools of nodes
    ~ThreadNodeCache();

    // Prevent copying or moving
    ThreadNodeCache(const ThreadNodeCache&) = delete;
    ThreadNodeCache(ThreadNodeCache&&) = delete;
    ThreadNodeCache& operator=(const ThreadNodeCache&) = delete;
    ThreadNodeCache& operator=(ThreadNodeCache&&) = delete;

    // Ask global cache for a free node
    Node* askGlobalForNode();
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
Index NodeAllocator<Node, NodesPerBlock>::tryRecycle(
    Node* saved, Node*& holdFree, Node*& holdSave,
    std::vector<const void*>& snapshot) noexcept {
  // No saved nodes, early return to prevent costly work
  if (!saved) {
    return 0;
  }

  // Ask hazard manager for an updated look at all of the pointers currently in
  // use
  snapshot.clear();
  ThreadHazardManager::getActivePointers(snapshot);

  // Sort and deduplicate
  std::ranges::sort(snapshot);
  snapshot.erase(std::ranges::unique(snapshot).begin(), snapshot.end());

  // Number of nodes that could not be put on the free list still
  Index saveCnt{0};

  while (saved) {
    Node* const curr{saved};
    saved = saved->_internal.next;

    // If not currently in-use, we can safely reset current node
    if (!std::ranges::binary_search(snapshot, curr)) {
      curr->reset();
      curr->_internal.next = holdFree;
      holdFree = curr;
    } else {
      // Otherwise, preserve back on the save list
      ++saveCnt;
      curr->_internal.next = holdSave;
      holdSave = curr;
    }
  }

  return saveCnt;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches::allocateBlock() {
  static_assert(NodesPerBlock > 0, "NodesPerBlock must be non-negative");

  // Allocate a full block of nodes (we do this to minimize the number of
  // times malloc has to be called)
  Node* block{new Node[NodesPerBlock]};
  TW_DEBUG_ONLY(nAllocs_.fetch_add(1, MemoryOrder::relaxed););

  // Reset performs a "true" value initialization of the nodes and then chain
  // the nodes together
  for (Index i{0}; i + 1 < NodesPerBlock; ++i) {
    block[i].reset();
    block[i]._internal.next = block + i + 1;
  }

  // Reset the last node
  block[NodesPerBlock - 1].reset();

  // Mark the absolute start of this OS allocation chunk so we know which
  // nodes to free in the destructor
  block[0]._internal.isBlockStart = true;

  // Return the entire block to the calling thread
  return block;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches::GlobalNodeCaches()
    : freeHead_{allocateBlock()} {}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches::~GlobalNodeCaches() {
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

#ifndef TW_NDEBUG
  Index nDealloc{0};
#endif

  while (blockStarts) {
    // ReSharper disable once CppLocalVariableMayBeConst
    Node* const curr{blockStarts};
    blockStarts = blockStarts->_internal.next;
    delete[] curr;
    TW_DEBUG_ONLY(++nDealloc;);
  }

  TW_ASSERT(nAllocs_.load(MemoryOrder::relaxed) == nDealloc,
            "Number of deleted block heads does not match the number of "
            "allocated block head.");
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
      // this CAS (profiling demonstrated better performance with strong CAS)
      if (freeHead_.compare_exchange_strong(freeNode, freeNode->_internal.next,
                                            MemoryOrder::acquire,
                                            MemoryOrder::relaxed)) {
        break;
      }
    }
  }

  // If we successfully detached a free node from the freelist
  if (freeNode) {
    freeNode->_internal.next = nullptr;
    return freeNode;
  }

  // Try recycling nodes from the save list
  if (Node* const saved{saveHead_.exchange(nullptr, MemoryOrder::acquire)}) {
    Node* holdFree{nullptr};  // Nodes that can be moved to free list
    Node* holdSave{nullptr};  // Nodes that remain in 'saved' state

    // Steal the calling thread's local cache snapshot to try to recycle nodes
    ThreadNodeCache& local{getThreadCaches()};
    tryRecycle(saved, holdFree, holdSave, local.hazardSnapshot_);

    // Return the still-pinned nodes back to the global save pool
    pushSave(holdSave);

    // If we uncovered safe nodes, take a block to return, cache the rest
    if (holdFree) {
      Node* const freeBlockHead{holdFree};
      Node* freeBlockTail{holdFree};
      Index nodeCnt{0};

      while (freeBlockTail && ++nodeCnt < NodesPerBlock) {
        freeBlockTail = freeBlockTail->_internal.next;
      }

      // Push the rest of the nodes back to the free list
      if (freeBlockTail) {
        pushFree(freeBlockTail->_internal.next);
        freeBlockTail->_internal.next = nullptr;
      }

      return freeBlockHead;
    }
  }

  // Fallback to OS allocation (this should happen rarely since we allocate
  // blocks of nodes at a time)
  return allocateBlock();
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
void NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches::pushSave(
    Node* const batchHead) {
  pushBatch(saveHead_, batchHead);
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
void NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches::pushFree(
    Node* const batchHead) {
  pushBatch(freeHead_, batchHead);
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
void NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches::pushBatch(
    std::atomic<Node*>& cacheHead, Node* const batchHead) {
  if (!batchHead) {
    return;
  }

  // Retrieve tail of the batch
  Node* batchTail{batchHead};

  while (batchTail->_internal.next) {
    batchTail = batchTail->_internal.next;
  }

  batchTail->_internal.next = cacheHead.load(MemoryOrder::relaxed);
  while (!cacheHead.compare_exchange_weak(batchTail->_internal.next, batchHead,
                                          MemoryOrder::release,
                                          MemoryOrder::relaxed));
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
std::shared_ptr<typename NodeAllocator<Node, NodesPerBlock>::GlobalNodeCaches>
NodeAllocator<Node, NodesPerBlock>::getGlobalCaches() {
  static auto global{std::make_shared<GlobalNodeCaches>()};
  return global;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::ThreadNodeCache::ThreadNodeCache() {
  // Reserve up to the max number of hazards to prevent heap allocations
  static_assert(std::is_same_v<std::underlying_type_t<HazardSlot>, Index>);
  hazardSnapshot_.reserve(static_cast<Index>(HazardSlot::COUNT) * kMaxThreads);
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::ThreadNodeCache::~ThreadNodeCache() {
  TW_ASSERT(
      globalCache_ != nullptr,
      "Global cache smart pointer was invalidated before thread teardown");
  Node* const saved{std::exchange(saveHead_, nullptr)};
  tryRecycle(saved, freeHead_, saveHead_, hazardSnapshot_);
  globalCache_->pushFree(freeHead_);
  globalCache_->pushSave(saveHead_);
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<Node, NodesPerBlock>::ThreadNodeCache::askGlobalForNode() {
  Node* const granted{globalCache_->askForNode()};
  TW_ASSERT(granted != nullptr, "Received a null allocation from global cache");
  Node* const rest{granted->_internal.next};
  granted->_internal.next = nullptr;
  TW_ASSERT(freeHead_ == nullptr,
            "askForGlobal called when free list is non-empty");
  freeHead_ = rest;  // safe (this is only ever called when free list is empty)
  return granted;
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
    Node* const saved{std::exchange(local.saveHead_, nullptr)};
    local.saveCnt_ = tryRecycle(saved, local.freeHead_, local.saveHead_,
                                local.hazardSnapshot_);
  }

  // Return node
  Node* node{nullptr};

  // If we have a free node available use it
  if (local.freeHead_) {
    node = local.freeHead_;
    local.freeHead_ = node->_internal.next;
    node->_internal.next = nullptr;
  } else {
    // Ask global pool for a node (if it has a free node, it will provide it, if
    // not this will perform a heap allocation which is not lock-free though
    // this fallback should occur infrequently
    node = local.askGlobalForNode();
  }

  TW_ASSERT(node != nullptr, "allocate() returning a nullptr");
  TW_ASSERT(node->_internal.next == nullptr,
            "allocated node still attached to list");
  return node;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
void NodeAllocator<Node, NodesPerBlock>::deallocate(Node* const node) noexcept {
  if (!node) {
    return;
  }

  // Push deallocated node to save list
  ThreadNodeCache& local{getThreadCaches()};
  TW_ASSERT(node->_internal.next == nullptr,
            "deallocate() received an internally linked node");
  node->_internal.next = local.saveHead_;
  local.saveHead_ = node;

  // Amortize the cost of trying to recycle/putting deallocated nodes back to
  // the free list
  if (++local.saveCnt_ >= NodesPerBlock) {
    Node* const saved{std::exchange(local.saveHead_, nullptr)};
    local.saveCnt_ = tryRecycle(saved, local.freeHead_, local.saveHead_,
                                local.hazardSnapshot_);
  }
}

}  // namespace ThreadWeave::Internal

#endif
