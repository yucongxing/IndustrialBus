# Simulated LayoutView and CMPSuite

This is the smallest runnable product-shaped IndustrialBus flow. It is pure
C++17 and does not depend on Qt.

`layoutview_sim` connects as `layout-view`, installs progress/completion
subscriptions, and submits an idempotent `cmp.simulation.run` Command to
`cmp-suite`. `cmpsuite_sim` uses the durable command journal, acknowledges the
accepted operation, publishes five progress events, persists Completed, and
publishes the final result DataRef.

## Dependency boundary

The two `main` files do not include or name BusClient, Protobuf, Topic types, or
the idempotency journal. They use two small application-facing pieces:

- `simple_logger.h`: a transport-neutral Logger dispatcher and `LogSink`
  interface with a thread-safe `ConsoleSink`;
- `simulation_app.h`: plain C++ options plus LayoutView client, CMPSuite
  service, and SimulationJob interfaces.

`simulated_app_support` implements those interfaces. All IndustrialBus and
Protobuf code is isolated in `bus_simulation_adapter.cpp` and the private
`simulation_topics.h` catalog. On connection, the adapter attaches a bounded
asynchronous `BusLogSink` to the same Logger and reuses the existing BusClient;
on shutdown it removes and drains the Sink before closing the client. Logger
records diagnostics, while the facade preserves explicit Command, progress,
completion, and failure semantics.

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
