enable_testing()
add_executable(
        dequeTest
        dequeTest.cpp
)

# Note: -fsanitize=thread (doesn't support std::atomic_fence)
set(SANITIZER_FLAGS
        -fsanitize=address
        -fsanitize=undefined
)

target_compile_options(
        dequeTest
        PRIVATE
        ${COMPILER_FLAGS}
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_options(
        dequeTest
        PRIVATE
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS}>"
)
target_link_libraries(
        dequeTest
        PRIVATE
        ThreadWeave
        GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(dequeTest)