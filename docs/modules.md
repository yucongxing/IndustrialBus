# Module responsibilities

The `IndustrialBus::*` names below are build-tree CMake aliases. Install and
export rules have not been implemented yet, so they are not installed package
targets.

## `api`

Owns the application-facing C++17 contracts and templates: `BusClient`, typed
topics and payload codecs, `ClientOptions`, destinations, errors/results,
executors, subscriptions, QoS values, and operation/progress types. It has no
linkable target of its own; its headers are exposed by Core, Client, and
Integration targets.

It does not implement transport, Hub routing, XDG startup, Protobuf generation,
Qt integration, or product algorithms. Core, Client, and Runtime tests exercise
the API indirectly.

## `core`

Owns transport-independent primitives: UUID/protocol identifiers, the operation
state machine, bounded priority queues, and the Linux append-only idempotency
journal. The journal owns durable registration, digest conflict detection,
execution claims, authoritative state updates, recovery, and sync-before-ack
semantics; it never executes an algorithm.

- Target: `industrial_bus_core` / `IndustrialBus::Core`
- Tests: Result/Error, identifiers, operation transitions, queue policy, and
  journal persistence/recovery in `core/tests/core_tests.cpp`

## `protocol`

Owns the Protobuf wire schemas and generated C++ for Envelope, system control
messages, operations, logging, DataRef, Layout, and CMP. System control messages
include service registration, subscription updates, Heartbeat/HeartbeatAck,
standard errors, and command acknowledgements.

It describes the wire contract only. It does not route, authenticate, send, or
apply business policy.

- Conditional target: `industrial_bus_protocol` / `IndustrialBus::Protocol`
- Coverage gap: historical fixture and schema compatibility tests are not yet
  implemented.

## `transport`

Owns the raw byte-frame SPI (`IConnection`, `IConnector`, `IListener`) and the
Loopback and ZeroMQ DEALER/ROUTER implementations. It owns connection-local
queues, frame delivery, and bounded shutdown behavior. A closed ZeroMQ
`RouterPeer` removes its identity mapping, while conditional erase and ROUTER
handover allow a new peer with the same routing identity without letting a late
close erase the replacement.

It does not understand Envelope, topics, services, tokens, discovery,
idempotency, or heartbeat policy.

- Targets: `IndustrialBus::TransportSPI`, `IndustrialBus::TransportLoopback`,
  and conditional `IndustrialBus::TransportZeroMQ`
- Tests: bidirectional frame and close propagation; real ZeroMQ IPC, bounded
  ROUTER shutdown, and same-routing-identity reconnection

## `bootstrap`

Owns Linux per-UID startup safety: validation and creation of the XDG runtime
directory, secure endpoint/token files, token generation, `flock` singleton
ownership, inherited lock adoption, and shell-free `posix_spawn` of CommHub.

It does not connect or authenticate a client, encode messages, route traffic,
or maintain sessions.

- Target: `industrial_bus_bootstrap` / `IndustrialBus::Bootstrap`
- Tests: permissions, symlink rejection, socket path bounds, lock inheritance,
  and launcher ownership transfer

## `client`

Owns the `BusClient` implementation and process-local singleton, callback
executor, pending request deadlines and late replies, subscription reference
counts, typed request/command handlers, and durable command acceptance through
the idempotency journal. On Linux it also owns endpoint/token discovery,
ConnectOnly/AutoStart selection, and assembly of the ZeroMQ runtime adapter.

It does not select Hub routes, listen on ROUTER, implement a transport backend,
provide Qt types, or execute product algorithms. Automatic reconnection after a
detected Hub loss is not implemented yet.

- Target: `industrial_bus_client` / `IndustrialBus::Client`
- Tests: client lifecycle, pending timeouts, subscriptions, generation changes,
  typed messaging, handler cancellation, durable command deduplication, and
  shutdown

Build-layout note: `runtime/src/envelope_codec.cpp` and
`runtime/src/transport_runtime_adapter.cpp` are currently compiled into Client
to avoid a Client/Runtime target cycle.

## `integration`

Owns thin application helpers: `OperationGuard`/thread-local operation context
and `ProgressThrottle`. These helpers publish lifecycle state and coalesce
progress without owning BusClient or transport lifetime.

It does not execute algorithms or currently provide Qt executor and logger
adapters.

- Target: `industrial_bus_integration` / `IndustrialBus::Integration`
- Coverage gap: there is no dedicated Integration GoogleTest target yet.

## `commhub`

Owns the transport- and codec-independent Hub state machine: session and service
registries, subscriptions, request return routes, message routing, QoS queues,
heartbeat timestamps, and race-safe session expiry that cleans all related
state.

It does not parse Protobuf, validate tokens, bind sockets, start processes,
persist messages, or implement cross-node routing.

- Target: `industrial_bus_commhub_core` / `IndustrialBus::CommHubCore`
- Tests: request/reply/event routing, error paths, QoS coalescing/overflow, and
  session expiry

`commhub/main.cpp` lives in this directory, but the `commhub` executable is
defined by `runtime/CMakeLists.txt` when ZeroMQ is enabled.

## `runtime`

Owns the concrete protocol/transport integration. Client-side
`TransportRuntimeAdapter` performs registration, subscription control,
Heartbeat transmission, HeartbeatAck validation, and Hub liveness timeout.
Server-side `CommHubServer` authenticates registration, maps peers to sessions,
validates and acknowledges heartbeats, routes frames, drains outbound queues,
and periodically expires stale sessions. Runtime also builds the `commhub`
process.

It does not define business APIs or algorithms, implement XDG file primitives,
or implement the ZeroMQ backend. Durable broker storage, cross-node operation,
and the Linux automatic reconnect supervisor remain future work.

- Target: `industrial_bus_runtime` / `IndustrialBus::Runtime`
- Executable: conditional `commhub`
- Tests: Envelope/Error codec, Loopback routing and authentication, heartbeat
  keepalive/loss/expiry, and real Linux discovery/AutoStart
