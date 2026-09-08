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
real ZeroMQ IPC. Client policy tests verify that a new connection generation
restores active subscriptions, fails pending Command/Request operations, and
does not replay either message. The simulated-app support target tests Logger
formatting and Sink fan-out with GoogleTest. Socket-denied sandboxes use
`GTEST_SKIP()` for the affected IPC case. Transport and process tests have
CTest timeouts. A
no-test core-only build sets both `BUILD_TESTING=OFF` and
`INDUSTRIAL_BUS_BUILD_TESTS=OFF` and therefore does not require GoogleTest.

`CMakeInstallConsumerTest` installs the current build into a clean staging
prefix, configures a separate consumer using only
`find_package(IndustrialBus CONFIG REQUIRED)`, compiles and links Client/Core/
Protocol, verifies the installed CommHub path, and runs the consumer. This
guards against accidentally exporting build-tree include or library paths.

Protocol tests validate Topic Catalog generation/freshness, Envelope limits and
cross-field contracts, zero-side-effect Hub rejection, released descriptor and
catalog compatibility, historical binary fixtures, and Protobuf unknown-field
round trips. See
[Topic Catalog and protocol validation](topic-catalog-protocol-validation.md)
for regeneration and baseline-update commands.

Current gaps are dedicated Integration helper tests, broader failure injection,
and stress/soak coverage.
