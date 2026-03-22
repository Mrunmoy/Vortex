cmake_minimum_required(VERSION 3.14)

project(Vortex
    VERSION 1.1.0
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

function(vortex_configure_target target_name)
    target_include_directories(${target_name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${target_name} PUBLIC cxx_std_17)

    if(NOT _backend STREQUAL "win32")
        find_package(Threads REQUIRED)
        target_link_libraries(${target_name} PRIVATE Threads::Threads)
    endif()
endfunction()

# ── Library ──────────────────────────────────────────────────────────
add_library(vortex STATIC
    src/backend_${_backend}.cpp
    src/CApi.cpp
)
vortex_configure_target(vortex)

add_library(vortex_shared SHARED
    src/backend_${_backend}.cpp
    src/CApi.cpp
)
vortex_configure_target(vortex_shared)
set_target_properties(vortex_shared PROPERTIES
    OUTPUT_NAME vortex
    VERSION     ${PROJECT_VERSION}
    SOVERSION   ${PROJECT_VERSION_MAJOR}
)

include(GNUInstallDirs)
install(TARGETS vortex vortex_shared
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES inc/vortex.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/vortex.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/vortex.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vortex.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/vortexConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/vortexConfig.cmake
    @ONLY
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/vortexConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/vortexConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/vortexConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vortex
)

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
