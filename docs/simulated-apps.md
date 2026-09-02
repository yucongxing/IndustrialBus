# Simulated LayoutView and CMPSuite

This is the smallest runnable product-shaped IndustrialBus flow. It is pure
C++17 and does not depend on Qt.

`layoutview_sim` connects as `layout-view`, installs progress/completion
subscriptions, and submits an idempotent `cmp.simulation.run` Command to
`cmp-suite`. `cmpsuite_sim` uses the durable command journal, acknowledges the
accepted operation, publishes five progress events, persists Completed, and
publishes the final result DataRef.

## Build

Use the same local dependency prefixes as the main project:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/deps/googletest-1.14.0"
cmake --build build -j
```

The examples are built by default. Set
`INDUSTRIAL_BUS_BUILD_SIMULATED_APPS=OFF` for library-only builds.

## Run

Both processes must use the same `XDG_RUNTIME_DIR`. The first process starts
CommHub automatically using the build-tree `commhub` path embedded by CMake.

Terminal 1:

```sh
export XDG_RUNTIME_DIR="/tmp/industrial-bus-demo-$UID"
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
./build/examples/simulated_apps/cmpsuite_sim --once
```

Terminal 2:

```sh
export XDG_RUNTIME_DIR="/tmp/industrial-bus-demo-$UID"
./build/examples/simulated_apps/layoutview_sim --timeout 15
```

Expected flow:

```text
LayoutView: requesting CMP operation ...
CMPSuite: accepted operation ... from layout-view
LayoutView: accepted by cmp-suite/...
LayoutView: progress polishing 20% - simulated CMP step 1/5
...
LayoutView: completed, result=file:///tmp/cmp-result-....oas
```

The default CMPSuite journal is
`/tmp/industrial-bus-cmpsuite-$UID.journal`; override it with `--journal`.
Both programs accept `--hub PATH` when a different CommHub executable is
needed.

## Deliberate simulation limits

- The input and result DataRef URIs are illustrative; no OASIS file is created.
- Checksums, TTL/release handling, cancellation, and failure injection are not
  implemented yet.
- Progress runs on a worker thread and is not adapted to a Qt event loop.
- CommHub remains a per-user background process after the two programs exit.
