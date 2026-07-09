#ifndef TW_PADDED_ATOMIC_INT_H
#define TW_PADDED_ATOMIC_INT_H
#include <threadweave/Constants.h>

#include <atomic>

// Helper class for testing on an atomic integer with padding to avoid false
// sharing
struct alignas(ThreadWeave::Internal::CacheLineSize) PaddedAtomicInt {
  std::atomic<int> val{0};

  int fetch_add(const int x, const std::memory_order order) {
    return val.fetch_add(x, order);
  }

  int fetch_sub(const int x, const std::memory_order order) {
    return val.fetch_sub(x, order);
  }

  int load(const std::memory_order order) const {
    return val.load(order);
  }

  void store(const int x, const std::memory_order order) {
    val.store(x, order);
  }
};
#endif  // TW_PADDED_ATOMIC_INT_H
