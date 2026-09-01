# IndustrialBus

IndustrialBus is a local-first C++17 communication runtime for LayoutView,
LayoutBoolean, CMPSuite, and related industrial desktop tools.

The first implementation targets Linux x86_64 with GCC 9+, Protobuf, and
ZeroMQ IPC. IndustrialBus itself has no Qt dependency; Qt applications inject
their own executor when callbacks must run on the UI thread.

The reviewed design baseline is in
[`industrial-bus-development-guide.md`](industrial-bus-development-guide.md).

## Build

Dependencies are installed locally and discovered with `find_package()`.
Tests use a locally installed GoogleTest Config Package; provide its prefix
through `CMAKE_PREFIX_PATH` when it is outside the default search path.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For a dependency-free core-only build, disable the external integrations:

```sh
cmake -S . -B build-core \
  -DINDUSTRIAL_BUS_ENABLE_PROTOBUF=OFF \
  -DINDUSTRIAL_BUS_ENABLE_ZEROMQ=OFF \
  -DBUILD_TESTING=OFF \
  -DINDUSTRIAL_BUS_BUILD_TESTS=OFF
```
