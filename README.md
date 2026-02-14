# ms-runloop

[![Build](https://github.com/Mrunmoy/ms-runloop/actions/workflows/ci.yml/badge.svg)](https://github.com/Mrunmoy/ms-runloop/actions/workflows/ci.yml)

Epoll-based event loop for C++17 with thread-safe callable posting.

> **Guided walkthrough** — [WalkthroughRunLoop.md](WalkthroughRunLoop.md) walks through every line of the implementation.

## Features

- **Pure event loop** — runs on a dedicated thread, no transport knowledge
- **Thread-safe posting** — `executeOnRunLoop()` queues work from any thread
- **fd source watching** — `addSource()` / `removeSource()` for readability events via epoll
- **FIFO ordering** — posted callables execute in submission order
- **Restartable** — `run()` can be called again after `stop()`
- **Deterministic shutdown** — `stop()` always terminates `run()`
- **14 unit tests** covering lifecycle, threading, ordering, fd sources, and restart

## Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| C++17 compiler | GCC 7+ / Clang 5+ | Language standard |
| CMake | 3.14+ | Build system |
| Python 3 | (optional) | Build script |
| Google Test | 1.14.0 | Unit tests (git submodule) |

## Quick Start

```cpp
#include "RunLoop.h"

ms::RunLoop loop;
loop.init("MyApp");

// Start on a background thread
std::thread t([&] { loop.run(); });

// Post work from any thread
loop.executeOnRunLoop([] {
    // runs on the loop thread
});

// Watch a file descriptor for readability
loop.addSource(fd, [&] {
    // called on the loop thread when fd is readable
});

loop.stop();
t.join();
```

## Building

```bash
# Clone
git clone --recursive https://github.com/Mrunmoy/ms-runloop.git
cd ms-runloop

# Build + test
python3 build.py -t

# Or directly with CMake
cmake -B build
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Build with examples
python3 build.py -e
```

## Using as a Submodule

```cmake
add_subdirectory(vendor/ms-runloop)
target_link_libraries(your_target PRIVATE ms-runloop)
```

When used as a submodule, tests and examples are not built.

## Project Structure

```
ms-runloop/
├── inc/
│   └── RunLoop.h              # Public header
├── src/
│   └── RunLoop.cpp            # Implementation (epoll + pipe wakeup)
├── test/
│   ├── CMakeLists.txt
│   ├── RunLoopTest.cpp        # 14 unit tests
│   └── vendor/googletest/     # Google Test (submodule)
├── example/
│   ├── CMakeLists.txt
│   ├── basic_usage.cpp        # API demo
│   └── event_notifier.cpp     # Multi-component event bus
├── .github/workflows/
│   └── ci.yml                 # GCC + Clang CI
├── CMakeLists.txt
├── build.py
├── WalkthroughRunLoop.md      # Implementation walkthrough
├── LICENSE
└── README.md
```

## License

[MIT](LICENSE)
