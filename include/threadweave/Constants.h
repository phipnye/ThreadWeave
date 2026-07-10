#ifndef TW_UTILS_H
#define TW_UTILS_H

#include <cstdint>
#include <new>

namespace ThreadWeave {

// Indexing integeral type
using Index = std::int64_t;

namespace Internal {
// Alignment to prevent false sharing
#ifdef CACHE_LINE_SIZE
static_assert(CACHE_LINE_SIZE > 0, "Cache line size should be positive");
static_assert(!(CACHE_LINE_SIZE & (CACHE_LINE_SIZE - 1)),
              "Cache line size should be a power of 2");
inline constexpr Index CacheLineSize{CACHE_LINE_SIZE};
#elif defined(__cpp_lib_hardware_interference_size)
inline constexpr Index CacheLineSize{
    std::hardware_destructive_interference_size};
#else
inline constexpr Index CacheLineSize{128};
#endif

// Maximum number of threads for fixed-sized hazard pointer pool and node
// wrapper pool
#ifdef MAX_THREADS
static_assert(MAX_THREADS > 0,
              "Max number of threads should be a positive value");
inline constexpr Index MaxThreads{MAX_THREADS};
#else
// Default value, user can set via macro
inline constexpr Index MaxThreads{64};
#endif

// Number of hazard pointers per thread (Treiber stack requires 1, MS Queue
// needs 2)
inline constexpr Index HazardsPerThread{2};

}  // namespace Internal
}  // namespace ThreadWeave

#endif
