#ifndef TW_HAZARD_H
#define TW_HAZARD_H

#include <threadweave/internal/utils.h>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ThreadWeave::Internal {

// Enum tracking the index of a hazard pointer
enum class HazardSlot : Index {
  Stack0 = 0,  // Stack only requires one hazard
  Queue0 = 0,  // Queue requires two hazards
  Queue1 = 1,
  Alloc2 = 2,  // Future requires one isolated hazard
  COUNT = 3    // Number of hazard pointers per thread
};

/**
 * Class to be used in a thread local context in which a given thread utilizes
 * the manager to acquire hazard pointers from a pool of them. Each
 * thread-manager pairing will obtain two hazard pointers and the user can
 * control for the number of hazard pointers necessary via the MAX_THREADS
 * macro.
 */
class ThreadHazardManager {
  struct alignas(kCacheLineSize) HazardPointerSlots {
    std::atomic<void*> hps[static_cast<Index>(HazardSlot::COUNT)];

    // Range-based loop support
    auto begin() noexcept(noexcept(std::begin(hps))) {
      return std::begin(hps);
    }
    auto end() noexcept(noexcept(std::end(hps))) {
      return std::end(hps);
    }
    auto cbegin() const noexcept(noexcept(std::cbegin(hps))) {
      return std::cbegin(hps);
    }
    auto cend() const noexcept(noexcept(std::cend(hps))) {
      return std::cend(hps);
    }
  };

  // Keep the high-frequency written hazard slots padded while thread ownership
  // data (IDs) in a separate dense array
  static inline HazardPointerSlots hpsPool[kMaxThreads]{};
  static inline std::atomic<std::thread::id> threadIds[kMaxThreads]{};

  // --- Data members
  Index poolIdx_;  // manager's thread slot index

 public:
  // --- Ctors, dtor, and assignement operators

  /**
   * Acquire a slot in the thread slot pool for current thread to indicate which
   * pointers it's using, helping us prevent ABA problem.
   */
  ThreadHazardManager();

  // Don't allow copy or move operations
  ThreadHazardManager(const ThreadHazardManager&) = delete;
  ThreadHazardManager(ThreadHazardManager&&) = delete;
  ThreadHazardManager& operator=(const ThreadHazardManager&) = delete;
  ThreadHazardManager& operator=(ThreadHazardManager&&) = delete;

  /**
   * Free this manager's resources in our pool so other threads can use it
   */
  ~ThreadHazardManager();

  // --- Member functions

  /**
   * Retrieve this managers's `idx`th hazard pointer
   * @param idx index of the manager's hazard pointer to retrieve
   * @return managers's `idx`th hazard pointer
   * @note Not marked as const because logically this is not const, the caller
   * retrieves an unprotected reference and likely will use it to store a new
   * memory address
   */
  std::atomic<void*>& getPointer(Index idx) noexcept;

  /**
   * Check if any threads are using node
   * @param nodePtr pointer to the node we want to check
   * @return true if nodePtr is used by any thread and false otherwise
   */
  static bool isPointerInUse(const void* nodePtr) noexcept;

  /**
   * Store a snapshot of the pointers actively in use
   * @param snapshot a vector of pointers to store the active pointers in
   */
  static void getActivePointers(std::vector<const void*>& snapshot) noexcept;
};

inline ThreadHazardManager::ThreadHazardManager() : poolIdx_{kMaxThreads} {
  // Search for the first available slot in our thread slot pool
  for (Index i{0}; i < kMaxThreads; ++i) {
    // Check ith slot to see if it's been claimed yet
    std::atomic<std::thread::id>& id{threadIds[i]};

    // If the ID is unset, claim this slot and store this thread's ID
    if (std::thread::id emptyId{}; id.compare_exchange_strong(
            emptyId, std::this_thread::get_id(), MemoryOrder::acquire,
            MemoryOrder::relaxed)) {
      poolIdx_ = i;
      break;
    }
  }

  // There are no available thread slots for the current thread, throw a
  // runtime error
  if (poolIdx_ == kMaxThreads) {
    throw std::runtime_error{"No available hazard pointers"};
  }
}

inline ThreadHazardManager::~ThreadHazardManager() {
  // Clear the hazard pointers before clearing the ID so other threads can use
  // this thread slot
  TW_ASSERT(poolIdx_ >= 0 && poolIdx_ < kMaxThreads,
            "Attempting to destroy uninitialized manager slot");
  auto& hps{hpsPool[poolIdx_]};

  for (auto& hp : hps) {
    hp.store(nullptr, MemoryOrder::relaxed);
  }

#ifndef TW_NDEBUG
  for (const auto& hp : hps) {
    TW_ASSERT(hp.load(MemoryOrder::relaxed) == nullptr,
              "Hazard pointer failed to clear during slot release");
  }
#endif

  threadIds[poolIdx_].store(std::thread::id{}, MemoryOrder::release);
}

inline std::atomic<void*>& ThreadHazardManager::getPointer(
    const Index idx) noexcept {
  TW_ASSERT(poolIdx_ >= 0 && poolIdx_ < kMaxThreads,
            "Invalid or uninitialized poolIdx_ in ThreadHazardManager");
  TW_ASSERT(idx >= 0 && idx < static_cast<Index>(HazardSlot::COUNT),
            "Hazard slot index out of bounds");
  return hpsPool[poolIdx_].hps[idx];
}

inline bool ThreadHazardManager::isPointerInUse(
    const void* const nodePtr) noexcept {
  if (!nodePtr) [[unlikely]] {
    return false;
  }

  // Pairs with the seq_cst fence in HazardGuard::acquirePointerWithHazard.
  // Without this, the removal that makes nodePtr eligible for recycling
  // (e.g. the CAS or exchange that unlinks it) and this scan are only ordered
  // by acquire/release, which permits an interleaving where a thread's
  // hazard-slot publish is invisible and thus both threads see stale memory
  // causing an ABA issue
  std::atomic_thread_fence(MemoryOrder::seq_cst);

  for (Index i{0}; i < kMaxThreads; ++i) {
    // Empty id indicates no use
    if (threadIds[i].load(MemoryOrder::relaxed) == std::thread::id{}) {
      continue;
    }

    // Otherwise, check if any pointers point to same memory location
    for (const auto& hp : hpsPool[i]) {
      if (hp.load(MemoryOrder::acquire) == nodePtr) {
        return true;
      }
    }
  }

  return false;
}

inline void ThreadHazardManager::getActivePointers(
    std::vector<const void*>& snapshot) noexcept {
  // Pairs with the seq_cst fence in HazardGuard::acquirePointerWithHazard.
  // Without this, the removal that makes nodePtr eligible for recycling
  // (e.g. the CAS or exchange that unlinks it) and this scan are only ordered
  // by acquire/release, which permits an interleaving where a thread's
  // hazard-slot publish is invisible and thus both threads see stale memory
  // causing an ABA issue
  std::atomic_thread_fence(MemoryOrder::seq_cst);

  for (Index i{0}; i < kMaxThreads; ++i) {
    // Empty id indicates no use
    if (threadIds[i].load(MemoryOrder::relaxed) == std::thread::id{}) {
      continue;
    }

    // Otherwise, check if any pointers point to same memory location
    for (const auto& hp : hpsPool[i]) {
      if (const void* p{hp.load(MemoryOrder::acquire)}) {
        snapshot.push_back(p);
      }
    }
  }
}

/**
 * Get current thread's `idx`th hazard pointer
 * @param idx index of the current thread's hazard pointer to retrieve
 * @return current thread's `idx`th hazard pointer
 */
inline std::atomic<void*>& getThreadHazardPointer(const Index idx) {
  thread_local ThreadHazardManager manager{};
  return manager.getPointer(idx);
}

/**
 * Check if any threads are using node
 * @param nodePtr pointer to the node we want to check
 * @return true if nodePtr is used by any thread and false otherwise
 */
inline bool anyThreadsUsingNode(const void* nodePtr) noexcept {
  return ThreadHazardManager::isPointerInUse(nodePtr);
}

/**
 * RAII guard for acquiring a pointer with hazard indicating use and a
 * destructor that clears the hazard once it goes out of scope indicating we're
 * no longer using the acquired pointer
 */
template <HazardSlot slot>
class HazardGuard {
  static_assert(std::is_same_v<std::underlying_type_t<HazardSlot>, Index>);
  static constexpr Index idx{static_cast<Index>(slot)};  // map slot to index
  static_assert(idx >= 0 && idx < static_cast<Index>(HazardSlot::COUNT),
                "Out-of-bounds hazard index");

 public:
  // --- Ctors, dtor, and assignment operations

  /**
   * Construct a hazard pointer RAII guard for clearing current thread's `idx`th
   * hazard pointer when going out of scope
   */
  HazardGuard() = default;

  // Prevent copy and move operations
  HazardGuard(const HazardGuard&) = delete;
  HazardGuard(HazardGuard&&) = delete;
  HazardGuard& operator=(const HazardGuard&) = delete;
  HazardGuard& operator=(HazardGuard&&) = delete;

  /**
   * Clear thread's `idx`th hazard pointer when going out of scope
   */
  ~HazardGuard();

  /**
   * Acquire a node pointer with the hazard indicating its use
   * @tparam T type that the atomic pointer points to
   * @param atomic atomic pointer of the resource we want to retrieve and store
   * in our hazard indicating current thread's use to other threads
   * @return a raw pointer to the memory location atomic points to
   */
  template <typename T>
  T* acquirePointerWithHazard(const std::atomic<T*>& atomic) const;
};

template <HazardSlot slot>
HazardGuard<slot>::~HazardGuard() {
  static_assert(idx >= 0 && idx < static_cast<Index>(HazardSlot::COUNT),
                "HazardGuard slot index out of bounds");
  std::atomic<void*>& hp{getThreadHazardPointer(idx)};
  hp.store(nullptr, MemoryOrder::release);
}

template <HazardSlot slot>
template <typename T>
T* HazardGuard<slot>::acquirePointerWithHazard(
    const std::atomic<T*>& atomic) const {
  static_assert(idx >= 0 && idx < static_cast<Index>(HazardSlot::COUNT),
                "HazardGuard slot index out of bounds");

  // Retrieve current thread's `idx`th hazard pointer
  std::atomic<void*>& hp{getThreadHazardPointer(idx)};

  // Continually fetch the atomic's pointer and try storing it in the hazard
  // pointer until we've successfully indicated use
  T* node{atomic.load(MemoryOrder::relaxed)};
  T* tmp{nullptr};

  // Memory allocator does not free heap memory until the end of the program,
  // thus our logic here is sound from the "pointer zapping" UB issue
  do {
    tmp = node;
    hp.store(tmp, MemoryOrder::release);

    // Pairs with the seq_cst fence in ThreadHazardManager::isPointerInUse.
    // This store must be visible to other threads before we reload below.
    // Otherwise, another thread could unlink tmp from its structure and scan
    // the hazard slots while this store is still sitting unseen, so neither
    // thread notices the other's operation. The fence forces this store and the
    // reload to line up in the same global order as that other thread's unlink
    // and then scan, so one of the two always sees what the other just did.
    std::atomic_thread_fence(std::memory_order::seq_cst);
    node = atomic.load(MemoryOrder::relaxed);
  } while (node != tmp);

  return node;
}

}  // namespace ThreadWeave::Internal

#endif
