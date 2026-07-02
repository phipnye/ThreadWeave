#ifndef TW_HAZARD_H
#define TW_HAZARD_H

#include <atomic>
#include <cstddef>
#include <thread>

namespace ThreadWeave::Internal {

class HazardPointer {
  // --- Pool of hazard pointers and their coupled IDs
  struct Couple {
    std::atomic<std::thread::id> id;
    std::atomic<void*> ptr;
  };

#ifndef MAX_NUM_HPS
  // Default value, user can set via macro
  static constexpr std::size_t maxNumHPs{64};
#else
  static_assert(MAX_NUM_HPS > 0,
                "Max number of hazard pointers should be a positive value");
  static constexpr std::size_t maxNumHPs{MAX_NUM_HPS};
#endif

  static inline Couple pool[maxNumHPs]{};

  // --- Data members
  std::size_t poolIdx_;

 public:
  // --- Ctors, Dtor, and assignement
  HazardPointer();

  // Don't allow copy or move operations
  HazardPointer(const HazardPointer&) = delete;
  HazardPointer(HazardPointer&&) = delete;
  HazardPointer& operator=(const HazardPointer&) = delete;
  HazardPointer& operator=(HazardPointer&&) = delete;

  // Free the pairing in our pool so other threads can use it
  ~HazardPointer();

  // --- Member functions

  // Return the pointer stored in our pool (note this does not modify the ID and
  // hence could be const but this violates logical constness and hence const
  // was omitted)
  [[nodiscard]] std::atomic<void*>& getPointer() noexcept;
  [[nodiscard]] static bool isPointerInUse(const void* nodePtr);
};

// Retrieve hazard pointer
std::atomic<void*>& getThreadHazardPointer();

// Check if any nodes are using ptr
bool anyThreadsUsingNode(const void* nodePtr);

// RAII guard for acquiring a pointer with hazard indicating use and a
// destructor that clears the hazard once it goes out of scope indicating we're
// no longer using the acquired pointer
class HazardGuard {
 public:
  HazardGuard() = default;
  HazardGuard(const HazardGuard&) = delete;
  HazardGuard(HazardGuard&&) = delete;
  HazardGuard& operator=(const HazardGuard&) = delete;
  HazardGuard& operator=(HazardGuard&&) = delete;
  ~HazardGuard();

  // Acquire a node pointer with the hazard indicating it's use
  // NOTE: Caller is in charge of clearing hazard pointer when pointer is no
  // longer in use so that the memory can be freed
  // Retrieve this thread's hazard pointer
  template <typename T>
  T* acquirePointerWithHazard(const std::atomic<T*>& atomic) const {
    std::atomic<void*>& hp{getThreadHazardPointer()};

    // Continually fetch the atomic's pointer and try storing it in the hazard
    // pointer until we've successfully claimed use
    T* node{atomic.load(std::memory_order::relaxed)};
    T* tmp{nullptr};

    do {
      tmp = node;
      hp.store(tmp, std::memory_order::seq_cst);
      node = atomic.load(std::memory_order::acquire);
    } while (node != tmp);

    return node;
  }
};

}  // namespace ThreadWeave::Internal

#endif
