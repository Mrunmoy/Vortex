# Owns googletest and all test targets.
#
# If it hasn't been cloned, print a helpful message and skip.

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/googletest/CMakeLists.txt")
    message(STATUS "ms-runloop: test/vendor/googletest not found, skipping tests. "
                   "Run: git submodule update --init --recursive")
    return()
endif()

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory(vendor/googletest)

add_executable(runloop_tests
    RunLoopTest.cpp
)
target_link_libraries(runloop_tests PRIVATE ms-runloop GTest::gtest_main pthread)

include(GoogleTest)
gtest_discover_tests(runloop_tests)
