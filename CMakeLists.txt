cmake_minimum_required(VERSION 3.14)

project(ms-runloop
    LANGUAGES CXX
    DESCRIPTION "Cross-platform event loop with thread-safe callable posting"
)

# ── Platform backend selection ───────────────────────────────────────
if(DEFINED MS_RUNLOOP_BACKEND)
    set(_backend ${MS_RUNLOOP_BACKEND})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_backend "epoll")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_backend "kqueue")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(_backend "win32")
else()
    set(_backend "stub")
endif()

message(STATUS "ms-runloop: using backend '${_backend}'")

# ── Library ──────────────────────────────────────────────────────────
add_library(ms-runloop src/backend_${_backend}.cpp)
target_include_directories(ms-runloop PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(ms-runloop PUBLIC cxx_std_17)

if(NOT _backend STREQUAL "win32")
    find_package(Threads REQUIRED)
    target_link_libraries(ms-runloop PRIVATE Threads::Threads)
endif()

# ── Tests and examples (only when building standalone) ───────────────
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(test)

    option(MS_RUNLOOP_BUILD_EXAMPLES "Build examples" OFF)
    if(MS_RUNLOOP_BUILD_EXAMPLES)
        add_subdirectory(example)
    endif()
endif()
