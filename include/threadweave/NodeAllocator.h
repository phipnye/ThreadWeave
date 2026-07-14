#ifndef TW_RETIRE_LIST_H
#define TW_RETIRE_LIST_H

#include <threadweave/Hazard.h>
#include <threadweave/Node.h>

#include <atomic>

namespace ThreadWeave::Internal {

/**
 * Class for retrieving and recycling nodes for a linked ...
 * @tparam Node a linked list node type
 */
template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
class NodeAllocator {
  // --- Enum supporting whether to operate on free list or to save for later
  // but don't reuse yet
  enum class StoreLocation : Index { free, save, COUNT };

  // Take head and see which nodes we can steal from the saved list and move to
  // the free list
  static void tryRecycle(Node* saved, Node*& holdFree,
                         Node*& holdSave) noexcept;

  // --- Global cache of nodes to share across threads
  class GlobalNodeCaches {
    // Number of nodes to allocate for at a time
    static constexpr Index NodesPerBlock{256};

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
  };

  // Retrieve the GlobalNodeCaches singleton
  static GlobalNodeCaches& getGlobalCaches();

  // --- Thread-local cache of nodes
  class ThreadNodeCache {
   public:
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
  };

  static ThreadNodeCache& getThreadCaches();

  // Helper to push a batch from thread local caches to global pool
  template <StoreLocation op>
  static void pushBatchToGlobal(Node* batchHead);

 public:
  // --- Ctors, dtor, and assignment operators

  /**
   * Default construct a retirement list
   */
  NodeAllocator() = default;

  /**
   * Trivially destruct a node allocator instance
   */
  ~NodeAllocator() = default;

  // Prevent copy and move operations
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

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
void NodeAllocator<Node>::tryRecycle(Node* saved, Node*& holdFree,
                                     Node*& holdSave) noexcept {
  while (saved) {
    Node* curr{saved};
    saved = saved->_internal.next;

    if (!Internal::anyThreadsUsingNode(curr)) {
      curr->_internal.next = holdFree;
      holdFree = curr;
    } else {
      curr->_internal.next = holdSave;
      holdSave = curr;
    }
  }
}

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
Node* NodeAllocator<Node>::GlobalNodeCaches::allocateBlock() {
  // Allocate a full block of nodes (we do this to minimize the number of
  // times malloc has to be called
  Node* block{static_cast<Node*>(::operator new(sizeof(Node) * NodesPerBlock))};

  // Mark the absolute start of this OS allocation chunk so we know which
  // nodes to free in the destructor
  block[0]._internal.isBlockStart = true;
  block[0]._internal.next = nullptr;  // isolate the node we are stealing

  // Chain the remaining nodes together and ensure their flags are false
  for (Index i{1}; i + 1 < NodesPerBlock; ++i) {
    block[i]._internal.next = &block[i + 1];
    block[i + 1]._internal.isBlockStart = false;
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
  batchTail->_internal.next = freeHead_.load(std::memory_order::relaxed);
  while (!freeHead_.compare_exchange_weak(batchTail->_internal.next, batchHead,
                                          std::memory_order::release,
                                          std::memory_order::relaxed)) {}

  // Hand the stolen first node directly back to the calling thread
  return &block[0];
}

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
NodeAllocator<Node>::GlobalNodeCaches::GlobalNodeCaches() {
  // Preallocate a block of the set number of nodes
  Node* initialNode{allocateBlock()};

  // Chain the "stolen" first node back onto the front of the free list
  initialNode->_internal.next = freeHead_.load(std::memory_order_relaxed);
  freeHead_.store(initialNode, std::memory_order_relaxed);
}

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
NodeAllocator<Node>::GlobalNodeCaches::~GlobalNodeCaches() noexcept {
  // Keep track of the nodes that are block starts so we can free memory
  // safely
  Node* blockStarts{nullptr};

  // Gather all of the nodes that are the starts of a block across all of
  // our lists
  for (Index i{0}; i < static_cast<Index>(StoreLocation::COUNT); ++i) {
    StoreLocation loc{static_cast<StoreLocation>(i)};
    Node* head{loc == StoreLocation::free
                   ? freeHead_.load(std::memory_order::relaxed)
                   : saveHead_.load(std::memory_order::relaxed)};

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
    Node* const curr{blockStarts};
    blockStarts = blockStarts->_internal.next;
    ::operator delete(curr);
  }
}

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
Node* NodeAllocator<Node>::GlobalNodeCaches::askForNode() {
  // First try taking from free list
  Node* node{freeHead_.load(std::memory_order::acquire)};

  while (node && !freeHead_.compare_exchange_weak(node, node->_internal.next,
                                                  std::memory_order::acquire,
                                                  std::memory_order::relaxed)) {
  }

  // If we successfully detached a free node from the freelist
  if (node) {
    return node;
  }

  // Try recycling nodes from the save list
  // clang-format off
      if (Node* saved{
                  saveHead_.exchange(nullptr, std::memory_order::acquire)}) {
    // clang-format on
    Node* holdFree{nullptr};  // Nodes that can be moved to free list
    Node* holdSave{nullptr};  // Nodes that remain in 'saved' state

    // Use the consolidated helper to sort the stolen global nodes
    tryRecycle(saved, holdFree, holdSave);

    // Return the still-pinned nodes back to the global save pool
    pushBatchToGlobal<StoreLocation::save>(holdSave);

    // If we uncovered safe nodes, peel one off to return, cache the rest
    if (holdFree) {
      node = holdFree;
      holdFree = holdFree->_internal.next;
      node->_internal.next = nullptr;
      pushBatchToGlobal<StoreLocation::free>(holdFree);
      return node;
    }
  }

  // Fallback to OS allocation (this should happen rarely since we allocate
  // blocks of nodes at a time)
  return allocateBlock();
}

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
NodeAllocator<Node>::GlobalNodeCaches& NodeAllocator<Node>::getGlobalCaches() {
  static GlobalNodeCaches global{};
  return global;
}

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
NodeAllocator<Node>::ThreadNodeCache::~ThreadNodeCache() {
  pushBatchToGlobal<StoreLocation::free>(freeHead_);
  Node* holdFree{nullptr};  // nodes that are now free
  Node* holdSave{nullptr};  // nodes that need to remain saved for later
  tryRecycle(saveHead_, holdFree, holdSave);
  pushBatchToGlobal<StoreLocation::free>(holdFree);
  pushBatchToGlobal<StoreLocation::save>(holdSave);
}

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
NodeAllocator<Node>::ThreadNodeCache& NodeAllocator<Node>::getThreadCaches() {
  thread_local ThreadNodeCache cache{};
  return cache;
}

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
template <typename NodeAllocator<Node>::StoreLocation op>
void NodeAllocator<Node>::pushBatchToGlobal(Node* batchHead) {
  if (!batchHead) {
    return;
  }

  // Retrieve the tail of the batch
  Node* batchTail{batchHead};

  while (batchTail->_internal.next) {
    batchTail = batchTail->_internal.next;
  }

  // Retrieve the list we want to push the batch to
  GlobalNodeCaches& global{getGlobalCaches()};
  std::atomic<Node*>& cacheHead{op == StoreLocation::free ? global.freeHead_
                                                          : global.saveHead_};

  // CAS loop until batch is successfully pushed
  batchTail->_internal.next = cacheHead.load(std::memory_order::relaxed);
  while (!cacheHead.compare_exchange_weak(batchTail->_internal.next, batchTail,
                                          std::memory_order::release,
                                          std::memory_order::relaxed));
}

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
Node* NodeAllocator<Node>::allocate() {
  ThreadNodeCache& local{getThreadCaches()};

  // Try recycling local nodes if necessary
  if (!local.freeHead_) {
    // Isolate the current local saved chain
    Node* saved{local.saveHead_};
    local.saveHead_ = nullptr;
    tryRecycle(saved, local.freeHead_, local.saveHead_);
  }

  // If we have a free node available use it
  if (local.freeHead_) {
    Node* node{local.freeHead_};
    local.freeHead_ = node->_internal.next;
    return node;
  }

  // Try asking global pool for a node (if it has a free node, it will provide
  // it, if not this will perform a heap allocation which is not lock-free
  // however hopefully
  return getGlobalCaches().askForNode();
}

template <typename Node>
  requires(HasInternalNextPointer<Node> && HasInternalBlockStartField<Node>)
void NodeAllocator<Node>::deallocate(Node* node) noexcept {
  if (!node) {
    return;
  }

  // If no threads are using node, we can immediately put it back on the free
  // list, otherwise we have to save it for later
  ThreadNodeCache& local{getThreadCaches()};
  Node*& cacheHead{Internal::anyThreadsUsingNode(node) ? local.saveHead_
                                                       : local.freeHead_};
  node->_internal.next = cacheHead;
  cacheHead = node;
}

}  // namespace ThreadWeave::Internal

#endif
