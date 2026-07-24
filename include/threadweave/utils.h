#ifndef TW_UTILS_H
#define TW_UTILS_H

#include <atomic>
#include <cstdint>
#include <new>

namespace ThreadWeave {

// Indexing integeral type
using Index = std::int64_t;

namespace Internal {
// Alignment to prevent false sharing
#ifdef TW_CACHE_LINE_SIZE
static_assert(TW_CACHE_LINE_SIZE > 0, "Cache line size should be positive");
// ReSharper disable once CppCompileTimeConstantCanBeReplacedWithBooleanConstant
static_assert(!(TW_CACHE_LINE_SIZE & (TW_CACHE_LINE_SIZE - 1)),
              "Cache line size should be a power of 2");
inline constexpr Index CacheLineSize{TW_CACHE_LINE_SIZE};
#elif defined(__cpp_lib_hardware_interference_size)
inline constexpr Index CacheLineSize{
    std::hardware_destructive_interference_size};
#else
inline constexpr Index CacheLineSize{128};
#endif

// Maximum number of threads for fixed-sized hazard pointer pool and node
// wrapper pool
#ifdef TW_MAX_THREADS
static_assert(TW_MAX_THREADS > 0,
              "Max number of threads should be a positive value");
inline constexpr Index MaxThreads{TW_MAX_THREADS};
#else
// Default value, user can set via macro
inline constexpr Index MaxThreads{64};
#endif

}  // namespace Internal

// Allow sequentially consistent in debug mode
namespace MemoryOrder {
#if !defined(NDEBUG) && defined(TW_DEBUG_SEQ_CST)
inline constexpr std::memory_order relaxed = std::memory_order_seq_cst;
inline constexpr std::memory_order consume = std::memory_order_seq_cst;
inline constexpr std::memory_order acquire = std::memory_order_seq_cst;
inline constexpr std::memory_order release = std::memory_order_seq_cst;
inline constexpr std::memory_order acq_rel = std::memory_order_seq_cst;
inline constexpr std::memory_order seq_cst = std::memory_order_seq_cst;
#else
inline constexpr std::memory_order relaxed = std::memory_order_relaxed;
inline constexpr std::memory_order consume = std::memory_order_consume;
inline constexpr std::memory_order acquire = std::memory_order_acquire;
inline constexpr std::memory_order release = std::memory_order_release;
inline constexpr std::memory_order acq_rel = std::memory_order_acq_rel;
inline constexpr std::memory_order seq_cst = std::memory_order_seq_cst;
#endif
}  // namespace MemoryOrder

}  // namespace ThreadWeave

#endif
