# Tests

Unit tests for ms-runloop, using [Google Test](https://github.com/google/googletest) v1.14.0.

## Prerequisites

Google Test is a git submodule under `test/vendor/googletest`. Clone with `--recursive` or run:

```bash
git submodule update --init --recursive
```

## Running tests

```bash
# From the project root:
python3 build.py -t

# Or with CMake directly:
cmake -B build && cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Test file

| File | What it tests |
|------|---------------|
| `RunLoopTest.cpp` | 9 tests covering the full RunLoop lifecycle: init/name, run/stop, stop-before-run, stop-from-callable, destructor cleanup, executeOnRunLoop thread affinity, multi-thread posting (4 threads x 25 posts), FIFO ordering (50 sequential posts), and restart-after-stop. |
