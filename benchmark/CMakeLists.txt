add_executable(BenchProgram ThreadPool.cpp)
target_link_libraries(BenchProgram PRIVATE ThreadWeave benchmark::benchmark)
