# Repository Guidelines

## Project Structure & Module Organization
`inc/RunLoop.h` exposes the public API. Platform backends live in `src/backend_*.cpp` and are selected in the root `CMakeLists.txt` via `VORTEX_BACKEND` (the older `MS_RUNLOOP_BACKEND` name is still accepted). Tests live in `test/`, with GoogleTest vendored as the submodule `test/vendor/googletest`. Examples are in `example/`, and `WalkthroughRunLoop.md` explains the implementation in detail.

## Build, Test, and Development Commands
Use the helper script for the standard workflow:

```bash
python3 build.py        # configure + build in ./build
python3 build.py -t     # build and run ctest
python3 build.py -e     # build examples
python3 build.py -c -t  # clean rebuild and run tests
```

Direct CMake also works:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Initialize submodules before testing: `git submodule update --init --recursive`.

## Coding Style & Naming Conventions
Target C++17. Follow the existing style: 4-space indentation, opening braces on their own lines, and standard library types fully qualified. Keep public types in the `vortex` namespace. Use `CamelCase` for classes and test suites (`RunLoop`, `RunLoopGuard`), `lowerCamelCase` for methods (`executeOnRunLoop`, `addSource`), and `m_` prefixes for private members. Match the current file naming scheme: platform files use `backend_<platform>.cpp`.

## Testing Guidelines
Tests use GoogleTest and are registered through `gtest_discover_tests`. Add new cases to `test/RunLoopTest.cpp` unless a new translation unit is justified. Name tests `TEST(RunLoopTest, BehaviorName)` and prefer behavior-focused cases covering thread safety, restartability, and backend-specific source handling. Run `python3 build.py -t` before opening a PR.

## Commit & Pull Request Guidelines
Recent commits use short, imperative subjects such as `Add cross-platform backend abstraction (#3)` and `Fix threading, error handling, and safety bugs in RunLoop (#2)`. Keep subjects concise, capitalized, and action-oriented. PRs should include the behavior changed, relevant issue link, platforms/backends affected, and the exact build/test command you ran. Include example output only when changing user-facing examples or docs.

## Configuration Notes
Standalone builds enable tests automatically; examples require `-DVORTEX_BUILD_EXAMPLES=ON`. Override backend selection only when needed, for example: `cmake -B build -DVORTEX_BACKEND=stub`.
