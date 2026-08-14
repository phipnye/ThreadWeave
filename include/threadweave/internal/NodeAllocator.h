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
 * Utility class for retrieving and recycling nodes for node-based structures
 * @tparam Node an AllocatorEligibleNode type (must have a reset method and
 * allocator info field)
 * @tparam NodesPerBlock number of nodes to allocate at a time (larger values
 * result in fewer heap allocations but could be more wasteful of memory)
 */
template <AllocatorEligibleNode Node, Index NodesPerBlock = 32>
class NodeAllocator {
  // Terminology note:
  // - Block refers to a chain of NodesPerBlock (except that a block returned to
  // the global pool may contain fewer than NodesPerBlock nodes if the local
  // pool's count wasn't a multiple of NodesPerBlock)
  // - Batch refers to a chain of an arbitrary number of nodes

  class GlobalNodePool {
   public:
    // Free nodes
    alignas(kCacheLineSize) std::atomic<Node*> freeList_{nullptr};

    // Nodes that can't be reused yet
    alignas(kCacheLineSize) std::atomic<Node*> retireList_{nullptr};

#ifndef TW_NDEBUG
    // Keep track of the number of allocations to make sure there are no leaks
    // (Note: this member must be initialized before free list gets initialized
    // with allocateBlock() which modifies this value)
    alignas(kCacheLineSize) mutable std::atomic<Index> nAllocs_{0};
#endif

    // Pre-allocate a fixed size of nodes
    GlobalNodePool();

    // Free all of the nodes in the global pool
    ~GlobalNodePool();

    // Prevent copying or moving
    GlobalNodePool(const GlobalNodePool&) = delete;
    GlobalNodePool(GlobalNodePool&&) = delete;
    GlobalNodePool& operator=(const GlobalNodePool&) = delete;
    GlobalNodePool& operator=(GlobalNodePool&&) = delete;

    // Orchestrate local pool block acquisition from global pool
    Node* acquireFreeBlock();

    // Push a single free block to free list
    void pushFreeBlock(Node* block);

    // Push an arbitrary batch of retired nodes to retirement list
    void pushRetireBatch(Node* batchHead);

   private:
    // Allocate a contiguous block of nodes
    Node* allocateBlock();

    // Pop an entire free block from global pool
    Node* popFreeBlock();

    // Flushes global retirement list, recycles safe nodes, and packages them
    // into blocks
    Node* recycleGlobalRetirementList();

    // Detaches up to NodesPerBlock nodes from the front of nodeList and
    // returns the block head, or nullptr if nodeList was already empty
    static Node* takeBlock(Node*& nodeList) noexcept;
  };

  class LocalNodePool {
   public:
    // Pointer to global pool singleton
    std::shared_ptr<GlobalNodePool> globalPool_{getGlobalPool()};

    // Thread local list of free nodes
    Node* freeList_{nullptr};

    // Thread local retirement list of nodes that cannot be recycled yet
    Node* retireList_{nullptr};

    // Deallocation informtion to delay recycling and amortize the cost
    std::vector<const void*> hazardSnapshot_{};
    Index retireCnt_{0};

    // Reserve space for hazard snapshots
    LocalNodePool();

    // Upon thread teardown, recycles retirement list and pushes all remaining
    // nodes to global pool
    ~LocalNodePool();

    // Prevent copying or moving
    LocalNodePool(const LocalNodePool&) = delete;
    LocalNodePool(LocalNodePool&&) = delete;
    LocalNodePool& operator=(const LocalNodePool&) = delete;
    LocalNodePool& operator=(LocalNodePool&&) = delete;

    // Requests an entire block from global pool
    void acquireFreeBlockFromGlobal();
  };

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
   * Retire a node back to the allocator so it can recycled for later use
   * @param node pointer to a node to recycle once all other threads are done
   * using it
   */
  static void deallocate(Node* node) noexcept;

 private:
  /**
   * Retrieve pointer to the GlobalNodePool singleton
   * @return a smart pointer to GlobalNodePool singleton
   */
  static std::shared_ptr<GlobalNodePool> getGlobalPool();

  /**
   * Retrieve a reference to the calling thread's local node pool
   * @return a reference to the calling thread's local node pool
   */
  static LocalNodePool& getLocalPool();

  /**
   * Try to recycle nodes from the retirement list to the free list
   * @return the number of nodes that were preserved in the retirement list
   */
  static Index tryRecycle(Node* retired, Node*& freeList, Node*& retireList,
                          std::vector<const void*>& snapshot) noexcept;
};

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::GlobalNodePool::GlobalNodePool() {
  // Pre-allocate enough blocks that threads racing in at startup don't all pile
  // up behind a single acquireFreeBlock() call
  const Index nInitialBlocks{
      std::clamp<Index>(std::thread::hardware_concurrency(), 2, 16)};
  Node* freeList{nullptr};

  for (Index i{0}; i < nInitialBlocks; ++i) {
    Node* const block{allocateBlock()};
    block->_internal.nextBlock = freeList;
    freeList = block;
  }

  freeList_.store(freeList, MemoryOrder::relaxed);
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::GlobalNodePool::~GlobalNodePool() {
  // Note, this destructor is completely safe at the time it gets called by the
  // use of shared pointers with reference counting. So long as a local pool
  // holds a reference, this destructor won't get called. This prevents free any
  // potential use-after-free issues or cases where the local destructors push
  // to the global pool after the global's destructor

  // Keep track of the nodes that are block starts so we can free memory
  // safely
  Node* blockStarts{nullptr};

  // Gather all of the nodes that are the starts of a block across all of
  // our lists
  Node* freeList{freeList_.load(MemoryOrder::relaxed)};

  while (freeList) {
    Node* block{freeList};
    freeList = freeList->_internal.nextBlock;

    while (block) {
      Node* const curr{block};
      block = block->_internal.nextFree;

      if (curr->_internal.isBlockStart) {
        // Use retire pointer for chaining the block starts
        curr->_internal.nextRetire = blockStarts;
        blockStarts = curr;
      }
    }
  }

  Node* retireList{retireList_.load(MemoryOrder::relaxed)};

  while (retireList) {
    Node* const curr{retireList};
    retireList = retireList->_internal.nextRetire;

    if (curr->_internal.isBlockStart) {
      curr->_internal.nextRetire = blockStarts;
      blockStarts = curr;
    }
  }

#ifndef TW_NDEBUG
  Index nDealloc{0};
#endif

  while (blockStarts) {
    Node* const curr{blockStarts};
    blockStarts = blockStarts->_internal.nextRetire;
    delete[] curr;
    TW_DEBUG_ONLY(++nDealloc;);
  }

  TW_ASSERT(nAllocs_.load(MemoryOrder::relaxed) == nDealloc,
            "Number of deleted block heads does not match the number of "
            "allocated block head.");
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<Node, NodesPerBlock>::GlobalNodePool::acquireFreeBlock() {
  // Try to directly take anything available first
  if (Node* const block{popFreeBlock()}) {
    return block;
  }

  // Otherwise, try to recycle and then try again
  if (Node* const block{recycleGlobalRetirementList()}) {
    return block;
  }

  // Fallback to OS allocation if nothing remains available
  return allocateBlock();
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
void NodeAllocator<Node, NodesPerBlock>::GlobalNodePool::pushFreeBlock(
    Node* const block) {
  if (!block) {
    return;
  }

  block->_internal.nextBlock = freeList_.load(MemoryOrder::relaxed);
  while (!freeList_.compare_exchange_weak(block->_internal.nextBlock, block,
                                          MemoryOrder::release,
                                          MemoryOrder::relaxed)) {}
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
void NodeAllocator<Node, NodesPerBlock>::GlobalNodePool::pushRetireBatch(
    Node* const batchHead) {
  if (!batchHead) {
    return;
  }

  // Acquire tail so we can chain the batch to the retirement list
  Node* batchTail{batchHead};

  while (batchTail->_internal.nextRetire) {
    batchTail = batchTail->_internal.nextRetire;
  }

  // Push entire batch to the retirement list
  batchTail->_internal.nextRetire = retireList_.load(MemoryOrder::relaxed);
  while (!retireList_.compare_exchange_weak(batchTail->_internal.nextRetire,
                                            batchHead, MemoryOrder::release,
                                            MemoryOrder::relaxed)) {}
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<Node, NodesPerBlock>::GlobalNodePool::allocateBlock() {
  // Allocate a full block of nodes (we do this to minimize the number of
  // times malloc has to be called)
  static_assert(NodesPerBlock > 0, "NodesPerBlock must be non-negative");
  Node* const block{new Node[NodesPerBlock]};
  TW_DEBUG_ONLY(nAllocs_.fetch_add(1, MemoryOrder::relaxed););

  // Reset performs a "true" value initialization of the nodes and then chain
  // the nodes together
  for (Index i{0}; i + 1 < NodesPerBlock; ++i) {
    block[i].reset();
    block[i]._internal.nextFree = block + i + 1;
  }

  // Reset the last node
  block[NodesPerBlock - 1].reset();

  // Mark the absolute start of this block so we know which nodes to free in the
  // destructor
  block[0]._internal.isBlockStart = true;

  // Return the entire block to the calling thread
  return block;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<Node, NodesPerBlock>::GlobalNodePool::popFreeBlock() {
  // Use a hazard guard to acquire the head of the free list
  HazardGuard<HazardSlot::Alloc2> guard{};

  while (true) {
    // Hazard ensures _internal.nextBlock is not overwritten until no longer
    // used
    Node* block{guard.acquirePointerWithHazard(freeList_)};

    // Global pool exhausted (no more nodes to acquire)
    if (!block) {
      return nullptr;
    }

    // CAS detaches an entire block of NodesPerBlock nodes
    if (freeList_.compare_exchange_strong(block, block->_internal.nextBlock,
                                          MemoryOrder::acquire,
                                          MemoryOrder::relaxed)) {
      block->_internal.nextBlock = nullptr;
      return block;
    }
  }
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<
    Node, NodesPerBlock>::GlobalNodePool::recycleGlobalRetirementList() {
  Node* const retired{retireList_.exchange(nullptr, MemoryOrder::acquire)};

  if (!retired) {
    return nullptr;
  }

  Node* holdFree{nullptr};    // nodes that can be moved to free list
  Node* holdRetire{nullptr};  // nodes that need to remain retired

  // Borrow calling thead's snapshot vector to store hazard snapshot
  LocalNodePool& local{getLocalPool()};
  tryRecycle(retired, holdFree, holdRetire, local.hazardSnapshot_);
  pushRetireBatch(holdRetire);

  // Hand the first recycled block straight back instead of pushing it to the
  // free list only to have the caller immediately CAS it back off
  Node* const firstBlock{takeBlock(holdFree)};

  while (Node* const block{takeBlock(holdFree)}) {
    pushFreeBlock(block);
  }

  return firstBlock;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<Node, NodesPerBlock>::GlobalNodePool::takeBlock(
    Node*& nodeList) noexcept {
  if (!nodeList) {
    return nullptr;
  }

  Node* const block{nodeList};
  Node* curr{nodeList};
  Index cnt{1};

  while (curr->_internal.nextFree && cnt < NodesPerBlock) {
    curr = curr->_internal.nextFree;
    ++cnt;
  }

  nodeList = curr->_internal.nextFree;
  curr->_internal.nextFree = nullptr;
  return block;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::LocalNodePool::LocalNodePool() {
  // Reserve up to the max number of hazards to prevent later heap allocations
  static_assert(std::is_same_v<std::underlying_type_t<HazardSlot>, Index>);
  hazardSnapshot_.reserve(static_cast<Index>(HazardSlot::COUNT) * kMaxThreads);
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::LocalNodePool::~LocalNodePool() {
  TW_ASSERT(globalPool_ != nullptr,
            "Global pool smart pointer was invalidated before thread teardown");

  // Recycle local retirement list
  Node* const retired{std::exchange(retireList_, nullptr)};
  tryRecycle(retired, freeList_, retireList_, hazardSnapshot_);

  // Return remaining retirement list nodes to global retirement list
  globalPool_->pushRetireBatch(retireList_);
  retireList_ = nullptr;

  // Slice local free list into blocks and push each to global free list
  while (freeList_) {
    Node* const block{freeList_};
    Node* curr{freeList_};
    Index cnt{1};

    while (curr->_internal.nextFree && cnt < NodesPerBlock) {
      curr = curr->_internal.nextFree;
      ++cnt;
    }

    freeList_ = curr->_internal.nextFree;
    curr->_internal.nextFree = nullptr;
    globalPool_->pushFreeBlock(block);
  }
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
void NodeAllocator<Node,
                   NodesPerBlock>::LocalNodePool::acquireFreeBlockFromGlobal() {
  TW_ASSERT(freeList_ == nullptr,
            "acquireBlockFromGlobal called when local free list is non-empty");
  freeList_ = globalPool_->acquireFreeBlock();
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Node* NodeAllocator<Node, NodesPerBlock>::allocate() {
  LocalNodePool& local{getLocalPool()};

  // Try recycling local nodes if necessary
  if (!local.freeList_) {
    Node* const retired{std::exchange(local.retireList_, nullptr)};
    local.retireCnt_ = tryRecycle(retired, local.freeList_, local.retireList_,
                                  local.hazardSnapshot_);
  }

  // If still no nodes available locally, ask global for a free block
  if (!local.freeList_) {
    local.acquireFreeBlockFromGlobal();
  }

  Node* const node{local.freeList_};
  TW_ASSERT(node != nullptr, "allocate() returning nullptr");
  local.freeList_ = node->_internal.nextFree;
  node->_internal.nextFree = nullptr;
  return node;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
void NodeAllocator<Node, NodesPerBlock>::deallocate(Node* const node) noexcept {
  if (!node) {
    return;
  }

  // Push deallocated node to retirement list
  LocalNodePool& local{getLocalPool()};
  node->_internal.nextRetire = local.retireList_;
  local.retireList_ = node;

  // Amortize the cost of trying to recycle deallocated nodes back to the free
  // list by waiting for a sufficient number of resources in retirement list
  if (++local.retireCnt_ >= NodesPerBlock) {
    Node* const retired{std::exchange(local.retireList_, nullptr)};
    local.retireCnt_ = tryRecycle(retired, local.freeList_, local.retireList_,
                                  local.hazardSnapshot_);
  }
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
std::shared_ptr<typename NodeAllocator<Node, NodesPerBlock>::GlobalNodePool>
NodeAllocator<Node, NodesPerBlock>::getGlobalPool() {
  static auto global{std::make_shared<GlobalNodePool>()};
  return global;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
NodeAllocator<Node, NodesPerBlock>::LocalNodePool&
NodeAllocator<Node, NodesPerBlock>::getLocalPool() {
  thread_local LocalNodePool local{};
  return local;
}

template <AllocatorEligibleNode Node, Index NodesPerBlock>
Index NodeAllocator<Node, NodesPerBlock>::tryRecycle(
    Node* retired, Node*& freeList, Node*& retireList,
    std::vector<const void*>& snapshot) noexcept {
  // No retired nodes, early return to prevent costly work
  if (!retired) {
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
  Index retireCnt{0};

  while (retired) {
    Node* const curr{retired};
    retired = retired->_internal.nextRetire;

    // If not currently in-use, we can safely reset current node and put it in
    // free list
    if (!std::ranges::binary_search(snapshot, curr)) {
      curr->reset();
      curr->_internal.nextRetire = nullptr;
      curr->_internal.nextFree = freeList;
      freeList = curr;
    } else {
      // Otherwise, put back in the retirement list
      ++retireCnt;
      curr->_internal.nextRetire = retireList;
      retireList = curr;
    }
  }

  return retireCnt;
}

}  // namespace ThreadWeave::Internal

#endif
