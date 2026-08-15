cmake_minimum_required(VERSION 3.23)
project(ThreadWeave VERSION 1.0 LANGUAGES CXX)

# --- Main target definition
add_library(ThreadWeave INTERFACE)
add_library(ThreadWeave::ThreadWeave ALIAS ThreadWeave)

# Set required language features without forcing global standards on downstream consumers
target_compile_features(ThreadWeave INTERFACE cxx_std_23)

# Headers layout
target_sources(ThreadWeave
        INTERFACE
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
        include/threadweave/ChaseLevDeque.h
        include/threadweave/MichaelScottQueue.h
        include/threadweave/ThreadPool.h
        include/threadweave/TreiberStack.h
        include/threadweave/VyukovQueue.h
        include/threadweave/internal/Future.h
        include/threadweave/internal/Hazard.h
        include/threadweave/internal/NodeAllocator.h
        include/threadweave/internal/Node.h
        include/threadweave/internal/Task.h
        include/threadweave/internal/utils.h
)

# Remove debug symbol in release mode
target_compile_definitions(ThreadWeave INTERFACE $<$<CONFIG:Release>:TW_NDEBUG>)

# Define cache line size instead of relying on std::hardware_destructive_interference_size
target_compile_definitions(ThreadWeave INTERFACE TW_CACHE_LINE_SIZE=64)

# Makes every atomic operation sequentially consistent
# target_compile_definitions(ThreadWeave INTERFACE TW_SEQ_CST)

# Sets the payload buffer size for holding function payloads
# target_compile_definitions(ThreadWeave INTERFACE TW_CALLABLE_STORAGE_SIZE=128)

# Set the maximum number of threads an application can expect to use for managing hazard pointer resource
# target_compile_definitions(ThreadWeave INTERFACE TW_MAX_THREADS=64)

# External dependency propagation
find_package(Threads REQUIRED)
target_link_libraries(ThreadWeave INTERFACE Threads::Threads)

# --- Top-Level project configurations (tests, examples, benchmarks)
if (PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    # Compiler warning flags applied strictly to testing/examples
    set(COMPILER_FLAGS
            -Wall
            -Wextra
            -Wshadow
            -Weffc++
            -Wconversion
            -pedantic-errors
            -Werror
            -Wno-error=sign-conversion
            -Wno-interference-size
            -fconcepts-diagnostics-depth=2
    )

    # Subdirectories
    add_subdirectory(benchmark)
    add_subdirectory(examples)
    add_subdirectory(profile)
    add_subdirectory(tests)

    # Dependencies
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
            googlebenchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG v1.9.5
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googlebenchmark)
    FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif ()
