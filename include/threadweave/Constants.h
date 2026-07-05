#ifndef TW_UTILS_H
#define TW_UTILS_H

#include <cstddef>
#include <new>

namespace ThreadWeave::Internal {

// Indexing integeral type
using Index = std::ptrdiff_t;

// Alignment to prevent false sharing
#ifdef __cpp_lib_hardware_interference_size
inline constexpr Index AlignSize{std::hardware_destructive_interference_size};
#else
inline constexpr Index AlignSize{128};
#endif

// Maximum number of threads for fixed-sized hazard pointer pool and node
// wrapper pool
#ifdef MAX_THREADS
static_assert(MAX_THREADS > 0,
              "Max number of threads should be a positive value");
inline constexpr Index maxThreads{MAX_THREADS};
#else
// Default value, user can set via macro
inline constexpr Index MaxThreads{64};
#endif

// Number of hazard pointers per thread
inline constexpr Index HazardsPerThread{2};

};  // namespace ThreadWeave::Internal

#endif
