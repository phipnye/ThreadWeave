# --- Comparisons benchmarks
add_executable(ComparisonsBenchmark comparisons_bm.cpp)
target_link_libraries(ComparisonsBenchmark PRIVATE ThreadWeave benchmark::benchmark)

# --- Speedup benchmarks
add_executable(SpeedupBenchmark speedup_bm.cpp)
target_link_libraries(SpeedupBenchmark PRIVATE ThreadWeave benchmark::benchmark)

# --- Latency benchmarks
add_executable(LatencyBenchmark latency_bm.cpp)
target_link_libraries(LatencyBenchmark PRIVATE ThreadWeave benchmark::benchmark)

# --- Performance benchmarks
add_executable(SortPerformanceBenchmark sortPerformance_bm.cpp)
target_include_directories(SortPerformanceBenchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(SortPerformanceBenchmark PRIVATE ThreadWeave benchmark::benchmark)

# --- Comparison dependencies

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_Declare(
        bshoshany_thread_pool
        GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
        GIT_TAG v5.1.0
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
        SYSTEM
)
FetchContent_MakeAvailable(bshoshany_thread_pool)
add_library(BS_thread_pool INTERFACE)
target_include_directories(BS_thread_pool INTERFACE ${bshoshany_thread_pool_SOURCE_DIR}/include)
target_link_libraries(ComparisonsBenchmark PRIVATE BS_thread_pool)

find_package(TBB REQUIRED COMPONENTS tbb)
target_link_libraries(SortPerformanceBenchmark PRIVATE TBB::tbb)
