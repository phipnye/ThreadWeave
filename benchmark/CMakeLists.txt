add_executable(BenchProgram ThreadPoolBench.cpp)
target_link_libraries(BenchProgram PRIVATE ThreadWeave benchmark::benchmark)

# --- Bshoshany thread pool for comparison

if(NOT TARGET BS_thread_pool)
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
endif()
target_link_libraries(BenchProgram PRIVATE BS_thread_pool)
