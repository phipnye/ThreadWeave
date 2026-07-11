cmake_minimum_required(VERSION 3.23)
project(ThreadWeave VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Compiler flags
set(COMPILER_FLAGS
        -Wall
        -Wextra
        -Wall
        -Wextra
        -Wshadow
        -Weffc++
        -Wconversion
        -pedantic-errors
        -Werror
        -Wno-interference-size
        -fconcepts-diagnostics-depth=2
)

# --- ThreadWeave library
add_library(ThreadWeave STATIC)
target_sources(ThreadWeave
        PRIVATE
        src/Hazard.cpp
        src/ThreadPool.cpp

        PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
        include/threadweave/ChaseLevDeque.h
        include/threadweave/Constants.h
        include/threadweave/Hazard.h
        include/threadweave/Node.h
        include/threadweave/Queue.h
        include/threadweave/NodeAllocator.h
        include/threadweave/Stack.h
        include/threadweave/ThreadPool.h
)
target_compile_options(ThreadWeave PRIVATE ${COMPILER_FLAGS})

# --- Third-party dependencies

include(FetchContent)
set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.5
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googlebenchmark googletest)

# --- Subdirectories
add_subdirectory(examples)
add_subdirectory(benchmark)
add_subdirectory(tests)
