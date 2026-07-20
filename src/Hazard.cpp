#include <threadweave/Hazard.h>
#include <threadweave/utils.h>

#include <atomic>
#include <stdexcept>
#include <thread>

namespace ThreadWeave::Internal {

ThreadHazardManager::ThreadHazardManager() : poolIdx_{MaxThreads} {
  // Search for the first available slot in our thread slot pool
  for (Index i{0}; i < MaxThreads; ++i) {
    // Check ith slot to see if it's been claimed yet
    auto& [id, ptrs]{slotsPool[i]};

    // If the ID is unset, claim this slot and store this thread's ID
    if (std::thread::id emptyId{}; id.compare_exchange_strong(
            emptyId, std::this_thread::get_id(), MemoryOrder::acquire,
            MemoryOrder::relaxed)) {
      poolIdx_ = i;
      break;
    }
  }

  // There are no available thread slots for the current thread, throw a runtime
  // error
  if (poolIdx_ == MaxThreads) {
    throw std::runtime_error{"No available hazard pointers"};
  }
}

ThreadHazardManager::~ThreadHazardManager() noexcept {
  // Clear the hazard pointers before clearing the ID so other threads can use
  // this thread slot
  auto& [id, ptrs]{slotsPool[poolIdx_]};

  for (auto& ptr : ptrs) {
    ptr.store(nullptr, MemoryOrder::relaxed);
  }

  id.store(std::thread::id{}, MemoryOrder::release);
}

// NOTE: Not marked as const because logically this is not const, the caller
// retrieves an unprotected reference and likely will use it to store a new
// memory address
std::atomic<void*>& ThreadHazardManager::getPointer(const Index idx) noexcept {
  return slotsPool[poolIdx_].ptr[idx];
}

bool ThreadHazardManager::isPointerInUse(const void* const nodePtr) noexcept {
  for (const auto& [id, ptrs] : slotsPool) {
    // Empty id indicates no use
    if (id.load(MemoryOrder::relaxed) == std::thread::id{}) {
      continue;
    }

    // Otherwise, check if any pointers point to same memory location
    for (const auto& ptr : ptrs) {
      if (ptr.load(MemoryOrder::acquire) == nodePtr) {
        return true;
      }
    }
  }

  return false;
}

std::atomic<void*>& getThreadHazardPointer(const Index idx) {
  thread_local ThreadHazardManager manager{};
  return manager.getPointer(idx);
}

bool anyThreadsUsingNode(const void* const nodePtr) noexcept {
  return ThreadHazardManager::isPointerInUse(nodePtr);
}

}  // namespace ThreadWeave::Internal
