#ifndef TW_HAZARD_H
#define TW_HAZARD_H

#include <threadweave/Constants.h>

#include <atomic>
#include <thread>

namespace ThreadWeave::Internal {

/**
 * Class to be used in a thread local context in which a given thread utilizes
 * the manager to acquire hazard pointers from a pool of them. Each
 * thread-manager pairing will obtain two hazard pointers and the user can
 * control for the number of hazard pointers necessary via the MAX_THREADS
 * macro.
 */
class ThreadHazardManager {
  // Pool of thread slots and their associated IDs
  struct ThreadSlots {
    std::atomic<std::thread::id> id;
    std::atomic<void*> ptr[HazardsPerThread];
  };
  static inline ThreadSlots slotsPool[MaxThreads]{};

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
  ~ThreadHazardManager() noexcept;

  // --- Member functions

  /**
   * Retrieve this managers's `idx`th hazard pointer
   * @param idx index of the manager's hazard pointer to retrieve
   * @return managers's `idx`th hazard pointer
   */
  std::atomic<void*>& getPointer(Index idx) noexcept;

  /**
   * Check if any threads are using node
   * @param nodePtr pointer to the node we want to check
   * @return true if nodePtr is used by any thread and false otherwise
   */
  static bool isPointerInUse(const void* nodePtr) noexcept;
};

/**
 * Get current thread's `idx`th hazard pointer
 * @param idx index of the current thread's hazard pointer to retrieve
 * @return current thread's `idx`th hazard pointer
 */
std::atomic<void*>& getThreadHazardPointer(Index idx);

/**
 * Check if any threads are using node
 * @param nodePtr pointer to the node we want to check
 * @return true if nodePtr is used by any thread and false otherwise
 */
bool anyThreadsUsingNode(const void* nodePtr) noexcept;

/**
 * RAII guard for acquiring a pointer with hazard indicating use and a
 * destructor that clears the hazard once it goes out of scope indicating we're
 * no longer using the acquired pointer
 */
template <Index idx>
class HazardGuard {
  static_assert(idx >= 0 && idx < HazardsPerThread,
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

template <Index idx>
HazardGuard<idx>::~HazardGuard() {
  std::atomic<void*>& hp{getThreadHazardPointer(idx)};
  hp.store(nullptr, std::memory_order::release);
}

template <Index idx>
template <typename T>
T* HazardGuard<idx>::acquirePointerWithHazard(
    const std::atomic<T*>& atomic) const {
  // Retrieve current thread's `idx`th hazard pointer
  std::atomic<void*>& hp{getThreadHazardPointer(idx)};

  // Continually fetch the atomic's pointer and try storing it in the hazard
  // pointer until we've successfully indicated use
  T* node{atomic.load(std::memory_order::relaxed)};
  T* tmp{nullptr};

  // TO DO: This code technically has UB, if a thread frees the memory tmp
  // points to and then when compare node != tmp, using tmp is UB per the
  // standard's "pointer zapping" rule
  do {
    // It's important the following operations occur in order and thus
    // sequential consistency is applied to make sure the store-load occurs in
    // program order globally
    // https://stackoverflow.com/questions/67693687/possible-orderings-with-memory-order-seq-cst-and-memory-order-release
    tmp = node;
    hp.store(tmp, std::memory_order::seq_cst);
    node = atomic.load(std::memory_order::seq_cst);
  } while (node != tmp);

  return node;
}

}  // namespace ThreadWeave::Internal

#endif
