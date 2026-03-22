# Owns googletest and all test targets.
#
# If it hasn't been cloned, print a helpful message and skip.

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/googletest/CMakeLists.txt")
    message(STATUS "Vortex: test/vendor/googletest not found, skipping tests. "
                   "Run: git submodule update --init --recursive")
    return()
endif()

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory(vendor/googletest)

add_executable(runloop_tests
    CApiTest.cpp
    RunLoopTest.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(runloop_tests PRIVATE vortex GTest::gtest_main Threads::Threads)

include(GoogleTest)
gtest_discover_tests(runloop_tests)
