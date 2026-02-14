cmake_minimum_required(VERSION 3.14)

project(ms-runloop
    LANGUAGES CXX
    DESCRIPTION "Epoll-based event loop with thread-safe callable posting"
)

# ── Library ──────────────────────────────────────────────────────────
add_library(ms-runloop src/RunLoop.cpp)
target_include_directories(ms-runloop PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(ms-runloop PUBLIC cxx_std_17)
target_link_libraries(ms-runloop PRIVATE pthread)

# ── Tests and examples (only when building standalone) ───────────────
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(test)

    option(MS_RUNLOOP_BUILD_EXAMPLES "Build examples" OFF)
    if(MS_RUNLOOP_BUILD_EXAMPLES)
        add_subdirectory(example)
    endif()
endif()
