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

  static constexpr std::size_t maxNumHPs{64};
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

}  // namespace ThreadWeave::Internal

#endif
