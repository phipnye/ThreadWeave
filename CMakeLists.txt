cmake_minimum_required(VERSION 3.23)
project(ThreadWeave VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
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
        src/scheduler.cpp

    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES include/threadweave/scheduler.h
)
target_compile_options(ThreadWeave PRIVATE ${COMPILER_FLAGS})

# --- Example program
add_executable(ExampleProgram)
target_sources(ExampleProgram PRIVATE examples/main.cpp)
target_compile_options(ExampleProgram PRIVATE ${COMPILER_FLAGS})
target_link_libraries(ExampleProgram PRIVATE ThreadWeave)
