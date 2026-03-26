# ── Google Benchmark competitive benchmarks for Vortex ───────────────
#
# Competitors (all optional, detected at configure time):
#   libuv        — libuv1-dev
#   libevent     — libevent-dev
#   Boost.Asio   — libboost-system-dev

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/benchmark/CMakeLists.txt")
    message(STATUS "Vortex: bench/vendor/benchmark not found, skipping benchmarks. "
                   "Run: git submodule update --init --recursive")
    return()
endif()

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/benchmark)

# ── Detect optional competitor libraries ─────────────────────────────
include(FindPkgConfig)

set(_bench_defines "")
set(_bench_libs benchmark::benchmark vortex Threads::Threads)

# libuv
pkg_check_modules(LIBUV QUIET libuv)
if(LIBUV_FOUND)
    message(STATUS "Vortex bench: found libuv ${LIBUV_VERSION}")
    list(APPEND _bench_defines HAS_LIBUV)
    list(APPEND _bench_libs ${LIBUV_LIBRARIES})
else()
    message(STATUS "Vortex bench: libuv not found — skipping libuv benchmarks")
endif()

# libevent (with pthreads support)
pkg_check_modules(LIBEVENT QUIET libevent libevent_pthreads)
if(LIBEVENT_FOUND)
    message(STATUS "Vortex bench: found libevent ${LIBEVENT_VERSION}")
    list(APPEND _bench_defines HAS_LIBEVENT)
    list(APPEND _bench_libs ${LIBEVENT_LIBRARIES})
else()
    message(STATUS "Vortex bench: libevent not found — skipping libevent benchmarks")
endif()

# Boost.Asio (header-only, needs Boost.System)
find_package(Boost QUIET COMPONENTS system)
if(Boost_FOUND)
    message(STATUS "Vortex bench: found Boost ${Boost_VERSION}")
    list(APPEND _bench_defines HAS_BOOST_ASIO)
    list(APPEND _bench_libs Boost::system)
else()
    message(STATUS "Vortex bench: Boost not found — skipping Asio benchmarks")
endif()

# ── Benchmark executable ─────────────────────────────────────────────
add_executable(vortex_benchmarks RunLoopBench.cpp)

target_compile_definitions(vortex_benchmarks PRIVATE ${_bench_defines})
target_link_libraries(vortex_benchmarks PRIVATE ${_bench_libs})
target_compile_features(vortex_benchmarks PRIVATE cxx_std_17)

if(LIBUV_FOUND)
    target_include_directories(vortex_benchmarks PRIVATE ${LIBUV_INCLUDE_DIRS})
endif()
if(LIBEVENT_FOUND)
    target_include_directories(vortex_benchmarks PRIVATE ${LIBEVENT_INCLUDE_DIRS})
endif()
