#ifndef TW_UTILS_H
#define TW_UTILS_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

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
inline constexpr Index kCacheLineSize{TW_CACHE_LINE_SIZE};
#elif defined(__cpp_lib_hardware_interference_size)
inline constexpr Index kCacheLineSize{
    std::hardware_destructive_interference_size};
#else
inline constexpr Index kCacheLineSize{128};
#endif

// Maximum number of threads for fixed-sized hazard pointer pool and node
// wrapper pool
#ifdef TW_MAX_THREADS
static_assert(TW_MAX_THREADS > 0,
              "Max number of threads should be a positive value");
inline constexpr Index kMaxThreads{TW_MAX_THREADS};
#else
// Default value, user can set via macro
inline constexpr Index kMaxThreads{32};
#endif

// Custom assertions
inline void assertionFailure(const char* expr, const char* file, const int line,
                             const char* msg) {
  std::cerr << "Assertion Failed: (" << expr << ")\n"
            << "File: " << file << ", Line: " << line << "\n";

  if (msg && msg[0] != '\0') {
    std::cerr << "Message: " << msg << "\n";
  }

  std::abort();
}

// Attempt to pause (pauses on GCC and clang for x86 architectures and
// std::this_thread::yield otherwise
inline void tryPause() noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__i386__) || defined(__x86_64__))
  __builtin_ia32_pause();
#else
  std::this_thread::yield();
#endif
}

}  // namespace Internal

// Allow sequentially consistent in debug mode
namespace MemoryOrder {
#if !defined(TW_NDEBUG) && defined(TW_SEQ_CST)
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

#ifndef TW_NDEBUG
#define TW_DEBUG_ONLY(...) \
  do {                     \
    __VA_ARGS__            \
  } while (0)
#define TW_ASSERT(expr, msg)                                                   \
  do {                                                                         \
    if (!(expr)) {                                                             \
      ThreadWeave::Internal::assertionFailure(#expr, __FILE__, __LINE__, msg); \
    }                                                                          \
  } while (0)
#else
#define TW_DEBUG_ONLY(...) ((void)0)
#define TW_ASSERT(expr, msg) ((void)0)
#endif

#endif
