#include <threadweave/Hazard.h>

#include <atomic>
#include <stdexcept>
#include <thread>

namespace ThreadWeave::Internal {

ThreadHazardManager::ThreadHazardManager() : poolIdx_{maxThreads} {
  // Search for the first available slot in our thread slot pool
  for (std::size_t i{0}; i < maxThreads; ++i) {
    // Check ith slot to see if it's been claimed yet
    auto& [id, ptrs]{pool[i]};

    // If the ID is unset, claim this slot and store this thread's ID
    if (std::thread::id emptyId{}; id.compare_exchange_strong(
            emptyId, std::this_thread::get_id(), std::memory_order::acquire,
            std::memory_order::relaxed)) {
      poolIdx_ = i;
      break;
    }
  }

  // There are no available thread slots for the current thread, throw a runtime
  // error
  if (poolIdx_ == maxThreads) {
    throw std::runtime_error{"No available hazard pointers"};
  }
}

ThreadHazardManager::~ThreadHazardManager() {
  // Clear the hazard pointers before clearing the ID so other threads can use
  // this thread slot
  auto& [id, ptrs]{pool[poolIdx_]};

  for (auto& ptr : ptrs) {
    ptr.store(nullptr, std::memory_order::relaxed);
  }

  id.store(std::thread::id{}, std::memory_order::release);
}

std::atomic<void*>& ThreadHazardManager::getPointer(
    const std::size_t idx) noexcept {
  return pool[poolIdx_].ptr[idx];
}

bool ThreadHazardManager::isPointerInUse(const void* const nodePtr) {
  for (const auto& [id, ptrs] : pool) {
    // Empty id indicates no use
    if (id.load(std::memory_order::relaxed) == std::thread::id{}) {
      continue;
    }

    // Otherwise, check if any pointers point to same memory location
    for (const auto& ptr : ptrs) {
      if (ptr.load(std::memory_order::acquire) == nodePtr) {
        return true;
      }
    }
  }

  return false;
}

std::atomic<void*>& getThreadHazardPointer(const std::size_t idx) {
  thread_local ThreadHazardManager manager{};
  return manager.getPointer(idx);
}

bool anyThreadsUsingNode(const void* const nodePtr) {
  return ThreadHazardManager::isPointerInUse(nodePtr);
}

HazardGuard::HazardGuard(const std::size_t idx) : idx_{idx} {}

HazardGuard::~HazardGuard() {
  std::atomic<void*>& hp{getThreadHazardPointer(idx_)};
  hp.store(nullptr, std::memory_order::release);
}

}  // namespace ThreadWeave::Internal