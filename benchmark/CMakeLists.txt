include(FetchContent)
set(BENCHMARK_ENABLE_TESTING OFF)

FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.5
)

FetchContent_MakeAvailable(googlebenchmark)
add_executable(BenchProgram ThreadPool.cpp)
target_link_libraries(BenchProgram PRIVATE ThreadWeave benchmark::benchmark)
