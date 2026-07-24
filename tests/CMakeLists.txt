enable_testing()
include(GoogleTest)

set(SANITIZER_FLAGS
        -fsanitize=address
        -fsanitize=undefined
)

# --- Chase-Lev Deque Tests

add_executable(ChaseLevDequeTests ChaseLevDequeTests.cpp)

# No TSan because of atomic thread fences
target_compile_options(ChaseLevDequeTests PRIVATE
        ${COMPILER_FLAGS}
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_options(ChaseLevDequeTests PRIVATE
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS}>"
)
target_link_libraries(ChaseLevDequeTests PRIVATE ThreadWeave GTest::gtest_main)
gtest_discover_tests(ChaseLevDequeTests)

# --- Treiber Stack Tests

add_executable(TreiberStackTests TreiberStackTests.cpp)
target_compile_options(TreiberStackTests PRIVATE
        ${COMPILER_FLAGS}
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_options(TreiberStackTests PRIVATE
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS}>"
)
target_link_libraries(TreiberStackTests PRIVATE ThreadWeave GTest::gtest_main)
gtest_discover_tests(TreiberStackTests)

# ---- Michael Scott Queue Tests

add_executable(MichaelScottQueueTests MichaelScottQueueTests.cpp)
target_compile_options(MichaelScottQueueTests PRIVATE
        ${COMPILER_FLAGS}
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_options(MichaelScottQueueTests PRIVATE
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_libraries(MichaelScottQueueTests PRIVATE ThreadWeave GTest::gtest_main)
gtest_discover_tests(MichaelScottQueueTests)

# --- Vyukov Queue Tests

add_executable(VyukovQueueTests VyukovQueueTests.cpp)
target_compile_options(VyukovQueueTests PRIVATE
        ${COMPILER_FLAGS}
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_options(VyukovQueueTests PRIVATE
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_libraries(VyukovQueueTests PRIVATE ThreadWeave GTest::gtest_main)
gtest_discover_tests(VyukovQueueTests)

# ---- Thread Pool Tests

add_executable(ThreadPoolTests ThreadPoolTests.cpp)
target_compile_options(ThreadPoolTests PRIVATE
        ${COMPILER_FLAGS}
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_options(ThreadPoolTests PRIVATE
        "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
)
target_link_libraries(ThreadPoolTests PRIVATE ThreadWeave GTest::gtest_main)
gtest_discover_tests(ThreadPoolTests)
