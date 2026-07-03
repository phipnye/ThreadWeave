add_executable(ExampleProgram)
target_sources(ExampleProgram PRIVATE main.cpp)
target_compile_options(ExampleProgram PRIVATE ${COMPILER_FLAGS})
target_link_libraries(ExampleProgram PRIVATE ThreadWeave)