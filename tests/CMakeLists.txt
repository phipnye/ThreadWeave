enable_testing()
include(GoogleTest)

set(TEST_SUITES
        ChaseLevDeque
        Future
        Hazard
        MichaelScottQueue
        NodeAllocator
        Task
        ThreadPool
        TreiberStack
        VyukovQueue
)

# No TSan because of atomic thread fences
set(SANITIZER_FLAGS
        -fsanitize=address
        -fsanitize=undefined
)

# --- Register Tests

foreach (SUITE_NAME IN LISTS TEST_SUITES)
    set(SUITE "${SUITE_NAME}Tests")
    add_executable("${SUITE}" "${SUITE}.cpp")
    target_compile_options("${SUITE}" PRIVATE
            ${COMPILER_FLAGS}
            "$<$<CONFIG:Debug>:${SANITIZER_FLAGS};-g>"
    )
    target_link_options("${SUITE}" PRIVATE
            "$<$<CONFIG:Debug>:${SANITIZER_FLAGS}>"
    )
    target_link_libraries("${SUITE}" PRIVATE ThreadWeave GTest::gtest_main)
    gtest_discover_tests("${SUITE}")
endforeach ()
