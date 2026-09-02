# Simulated LayoutView and CMPSuite

This is the smallest runnable product-shaped IndustrialBus flow. It is pure
C++17 and does not depend on Qt.

Each `cmpsuite_sim` owns a UUID channel, registers that UUID as its service
instance, and uses a channel-specific durable journal. `layoutview_sim` first
discovers online channels and then binds to exactly one of them. It installs
channel-specific progress/completion subscriptions and submits an idempotent
Command to that exact `cmp-suite/<channel>` instance. Several CMPSuite
processes can therefore run concurrently without sharing Commands or Events.

For a message-by-message explanation with architecture and sequence diagrams,
see [LayoutView and CMPSuite communication](simulated-communication.md).

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
IndustrialBus uses that standard per-user directory for its Unix IPC socket,
endpoint record, token, and startup lock. Applications normally inherit the
desktop/session value and do not need to set it. The `/tmp` value below is only
a self-contained demo fallback and must be private mode `0700`.

Terminal 1 (the generated channel is printed before startup):

```sh
export XDG_RUNTIME_DIR="/tmp/industrial-bus-demo-$UID"
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
./build/examples/simulated_apps/cmpsuite_sim --once
```

Alternatively, assign a stable channel explicitly. Reuse the same UUID and
journal path after a restart when the new process must represent the same
logical CMPSuite channel:

```sh
CHANNEL=11111111-1111-4111-8111-111111111111
./build/examples/simulated_apps/cmpsuite_sim --once --channel "$CHANNEL"
```

Terminal 2 first lists the live channels:

```sh
export XDG_RUNTIME_DIR="/tmp/industrial-bus-demo-$UID"
./build/examples/simulated_apps/layoutview_sim --list-channels
```

Then bind this LayoutView process to one channel:

```sh
./build/examples/simulated_apps/layoutview_sim \
  --channel 11111111-1111-4111-8111-111111111111 --timeout 15
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
`/tmp/industrial-bus-cmpsuite-$UID-<channel>.journal`; override it with
`--journal`. A custom journal belongs to one logical channel and must not be
reused with another channel ID. On startup, non-terminal records left by an
interrupted process are durably changed to `Failed/OwnerUnavailable`; terminal
records are preserved. Recovery does not replay Commands or offline Events.

Starting another CMPSuite with the same channel is rejected. With the default
journal it normally fails first on the journal file lock; with separate custom
journals CommHub rejects the duplicate `cmp-suite/<channel>` registration.
Both programs accept `--hub PATH` when a different CommHub executable is
needed.

## Deliberate simulation limits

- The input and result DataRef URIs are illustrative; no OASIS file is created.
- Checksums, TTL/release handling, cancellation, and failure injection are not
  implemented yet.
- Progress runs on a worker thread and is not adapted to a Qt event loop.
- Service discovery is an instantaneous local snapshot; `--list-channels`
  does not wait for a future CMPSuite to appear.
- Events and bus logs are best effort. If a channel Topic has no current
  subscriber, CommHub has no forwarding target and does not retain the Event.
- CommHub remains a per-user background process after the two programs exit.
