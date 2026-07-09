enable_testing()
include(GoogleTest)

set(SANITIZER_FLAGS
        -fsanitize=address
        -fsanitize=undefined
)

# --- Chase-Lev Deque Tests

add_executable(dequeTests dequeTest.cpp)

# No TSan because of atomic thread fences
target_compile_options(dequeTests PRIVATE
        ${COMPILER_FLAGS}
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_options(dequeTests PRIVATE
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS}>"
)
target_link_libraries(dequeTests PRIVATE ThreadWeave GTest::gtest_main)
gtest_discover_tests(dequeTests)

# --- Treiber Stack Tests

add_executable(stackTests stackTest.cpp)
target_compile_options(stackTests PRIVATE
        ${COMPILER_FLAGS}
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_options(stackTests PRIVATE
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS}>"
)
target_link_libraries(stackTests PRIVATE ThreadWeave GTest::gtest_main)
gtest_discover_tests(stackTests)
