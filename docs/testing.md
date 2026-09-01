# Testing

All C++ tests use GoogleTest 1.14-compatible APIs and are registered in CTest
with `gtest_discover_tests`. GoogleTest is a test-only, locally installed Config
Package dependency:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/local/dependency/prefixes
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The repository never downloads GoogleTest during configure or build. Machine
paths belong in `CMakeUserPresets.json` or the environment and are not committed.

The full suite covers Core, Bootstrap, Client, CommHub, Runtime, Loopback, and
real ZeroMQ IPC. Socket-denied sandboxes use `GTEST_SKIP()` for the affected IPC
case. Transport and process tests have CTest timeouts. A no-test core-only build
sets both `BUILD_TESTING=OFF` and `INDUSTRIAL_BUS_BUILD_TESTS=OFF` and therefore
does not require GoogleTest.

Current gaps are protocol compatibility fixtures, dedicated Integration helper
tests, failure injection, and stress/soak coverage.
