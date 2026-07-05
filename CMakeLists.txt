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
        FILES include/threadweave/ThreadPool.h
)
target_compile_options(ThreadWeave PRIVATE ${COMPILER_FLAGS})

# --- Subdirectories
add_subdirectory(examples)
add_subdirectory(benchmark)
