#include <threadweave/hazard.h>

#include <atomic>
#include <stdexcept>
#include <thread>

namespace ThreadWeave::Internal {

// Ctor
HazardPointer::HazardPointer() : poolIdx_{maxNumHPs} {
  // Search for the first available pointer available in our pool
  for (std::size_t i{0}; i < maxNumHPs; ++i) {
    auto& [id, ptr]{pool[i]};

    // If the ID is unset, claim this pairing and store this thread's ID
    if (std::thread::id oldId{}; id.compare_exchange_strong(
            oldId, std::this_thread::get_id(), MemoryOrder::acquire,
            MemoryOrder::relaxed)) {
      poolIdx_ = i;
      break;
    }
  }

  // There are no available pointers for the current thread, throw a runtime
  // error
  if (poolIdx_ == maxNumHPs) {
    throw std::runtime_error{"No available hazard pointers"};
  }
}

// Dtor
HazardPointer::~HazardPointer() {
  // Clear the memory location before clearing the ID so other threads can use
  // this pointer
  pool[poolIdx_].ptr.store(nullptr, MemoryOrder::relaxed);
  pool[poolIdx_].id.store(std::thread::id{}, MemoryOrder::release);
}

// Retrieve hazard pointer
// ReSharper disable once CppMemberFunctionMayBeConst
std::atomic<void*>& HazardPointer::getPointer() noexcept {
  return pool[poolIdx_].ptr;
}

// Get current thread's hazard pointer
std::atomic<void*>& getThreadHazardPointer() {
  thread_local HazardPointer hp{};
  return hp.getPointer();
}

// Check if any nodes are using ptr
bool HazardPointer::isPointerInUse(const void* const nodePtr) {
  for (auto& [_, ptr] : pool) {
    if (ptr.load(MemoryOrder::acquire) == nodePtr) {
      return true;
    }
  }

  return false;
}

// Check if any threads are using node
bool anyThreadsUsingNode(const void* const nodePtr) {
  return HazardPointer::isPointerInUse(nodePtr);
}

}  // namespace ThreadWeave::Internal