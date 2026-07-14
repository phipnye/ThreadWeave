add_executable(BenchProgram ThreadPoolBench.cpp)
target_link_libraries(BenchProgram PRIVATE ThreadWeave benchmark::benchmark)
