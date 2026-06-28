#include <threadweave/hazard.h>

namespace ThreadWeave::Internal {

// Ctor
HazardPointer::HazardPointer() : pairIdx_{maxNumHPs} {
  // Search for the first available pointer available in our pool
  for (std::size_t i{0}; i < maxNumHPs; ++i) {
    auto& [id, ptr]{pool[i]};

    // If the ID is unset, claim this pairing and store this thread's ID
    if (std::thread::id oldId{};
        id.compare_exchange_strong(oldId, std::this_thread::get_id())) {
      pairIdx_ = i;
      break;
    }
  }

  // There are no available pointers for the current thread, throw a runtime
  // error
  if (pairIdx_ == maxNumHPs) {
    throw std::runtime_error{"No available hazard pointers"};
  }
}

// Dtor
HazardPointer::~HazardPointer() {
  pool[pairIdx_].ptr.store(nullptr);
  pool[pairIdx_].id.store(std::thread::id{});
}

// Retrieve hazard pointer
std::atomic<void*>& HazardPointer::getPointer() const noexcept {
  return pool[pairIdx_].ptr;
}

// Get current thread's hazard pointer
std::atomic<void*>& getThreadHazardPointer() {
  thread_local HazardPointer hp{};
  return hp.getPointer();
}

// Check if any nodes are using ptr
bool HazardPointer::pointerInUse(const void* const nodePtr) {
  for (auto& [_, ptr] : pool) {
    if (ptr.load() == nodePtr) {
      return true;
    }
  }

  return false;
}

// Check if any threads are using node
bool anyThreadsUsingNode(const void* const nodePtr) {
  return HazardPointer::pointerInUse(nodePtr);
}

}  // namespace ThreadWeave::Internal