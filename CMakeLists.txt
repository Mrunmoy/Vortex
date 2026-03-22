cmake_minimum_required(VERSION 3.14)

project(Vortex
    LANGUAGES CXX
    DESCRIPTION "Cross-platform event loop with thread-safe callable posting"
)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Platform backend selection ───────────────────────────────────────
if(DEFINED VORTEX_BACKEND)
    set(_backend ${VORTEX_BACKEND})
elseif(DEFINED MS_RUNLOOP_BACKEND)
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

set(_valid_backends epoll kqueue win32 stub)
if(NOT _backend IN_LIST _valid_backends)
    message(FATAL_ERROR "Vortex: unknown backend '${_backend}'. "
                        "Valid options: ${_valid_backends}")
endif()

message(STATUS "Vortex: using backend '${_backend}'")

# ── Library ──────────────────────────────────────────────────────────
add_library(vortex src/backend_${_backend}.cpp)
target_include_directories(vortex PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(vortex PUBLIC cxx_std_17)

if(NOT _backend STREQUAL "win32")
    find_package(Threads REQUIRED)
    target_link_libraries(vortex PRIVATE Threads::Threads)
endif()

# ── Tests and examples (only when building standalone) ───────────────
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(test)

    option(VORTEX_BUILD_EXAMPLES "Build examples" OFF)
    if(NOT DEFINED VORTEX_BUILD_EXAMPLES AND DEFINED MS_RUNLOOP_BUILD_EXAMPLES)
        set(VORTEX_BUILD_EXAMPLES ${MS_RUNLOOP_BUILD_EXAMPLES})
    endif()
    if(VORTEX_BUILD_EXAMPLES)
        add_subdirectory(example)
    endif()
endif()
