# Runtime lifecycle

## Startup

1. `BusClient::connect` validates the service identity and preserves one live
   client per process.
2. The Linux adapter reads the XDG endpoint record and token. ConnectOnly never
   launches a process. AutoStart competes for the inherited startup lock,
   performs a second discovery check, and launches the trusted absolute CommHub
   executable only while owning that lock.
3. `TransportRuntimeAdapter` opens a DEALER connection and sends authenticated
   `ServiceRegistration`, including `SingletonPerUser` or `MultiInstance`.
4. The client becomes Connected only after a matching registration Ack verifies
   causation, protocol version, and the discovered Hub instance ID. The Ack
   also installs a revisioned snapshot of every registered service instance.

`SingletonPerUser` is the compatibility default: another online instance of
the same service name conflicts. `MultiInstance` permits several instances of
that service, but each `(service_name, instance_id)` pair remains unique and
multi-instance callers must use an explicit destination instance when route
selection would otherwise be ambiguous.

## Service directory

- `BusClient::services()` returns the runtime's current thread-safe snapshot.
- Registration adds an instance and clean disconnect removes it. CommHub
  broadcasts both changes with a monotonically increasing directory revision.
- Heartbeat expiry also removes the stale service and broadcasts that removal.
- A descriptor contains service name, instance ID, node ID, version, and a
  `local` flag. Here `local` means the service has the same node ID as the
  observing client; it does not mean "this client instance".
- The snapshot is discovery state, not a reservation. A service can disappear
  after it is listed, so exact routing may still return `ServiceUnavailable`.
- There is no durable directory or offline Event queue. A fresh registration
  snapshot reconstructs state after reconnect, but automatic Linux reconnect
  is not implemented yet.

## Heartbeat lease

- Default client heartbeat interval: 2 seconds.
- Default client liveness timeout: 6 seconds; it must be at least twice the
  non-zero interval.
- Default Hub session timeout: 6 seconds; the watchdog scans every 500 ms.
- The client sends `HEARTBEAT + SYSTEM_PAYLOAD + Heartbeat` on
  `system.heartbeat`, with a monotonic sequence and unique message ID.
- CommHub validates the registered source through HubCore, refreshes the lease
  using `steady_clock`, and replies with
  `REPLY + SYSTEM_PAYLOAD + HeartbeatAck`.
- The client accepts an Ack only when causation, sequence, and expected Hub
  instance match. Unix timestamps are diagnostic only.
- Any valid routed inbound business message also proves recent Hub activity.
- At timeout, the client atomically transitions once to Disconnected, detaches
  and closes the transport connection, and stops accepting new sends. Runtime
  worker and application callback exceptions are isolated from process-level
  thread termination. Command and Request messages are never replayed
  automatically.
- Connected notification completes before the heartbeat worker is allowed to
  publish a loss notification. A client that timed out must call `close()` to
  reap the completed worker before calling `start()` again; an immediate
  restart attempt is rejected instead of replacing a joinable thread.
- The Hub atomically removes expired sessions from the session registry and
  then cleans their service entry, return routes, QoS queues, and peer mapping.

Automatic Linux reconnection and subscription restoration after this
Disconnected transition are not implemented yet.

## Shutdown

BusClient stops accepting work, detaches receive/connection callbacks, cancels
subscriptions and handlers, fails pending operations, flushes, closes the
runtime, and then stops its executor. Runtime heartbeat and Hub maintenance
threads are signalled and joined without holding callback or connection locks.
CommHub then closes the listener and peers and removes its own discovery files.
