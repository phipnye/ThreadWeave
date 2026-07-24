add_executable(profileLatency profileLatency.cpp)
#target_compile_options(profileLatency PRIVATE ${COMPILER_FLAGS})
target_compile_options(profileLatency PRIVATE -g)
target_link_libraries(profileLatency PRIVATE ThreadWeave benchmark::benchmark)
