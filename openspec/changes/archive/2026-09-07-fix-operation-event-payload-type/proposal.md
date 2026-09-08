## Why

`BusClient::reportOperation()` labels serialized `OperationEvent` payloads as `industrial_bus.system.v1.OperationEvent`, although the Protobuf message is declared in `industrial_bus.operation.v1`. This metadata mismatch prevents correctly typed subscribers and future Hub catalog validation from recognizing operation events.

## What Changes

- Emit the canonical `industrial_bus.operation.v1.OperationEvent` full type name for operation events.
- Keep the existing `system.operation.event` routing topic and OperationEvent Protobuf schema unchanged.
- Add regression coverage that compares the emitted metadata with the Protobuf descriptor and verifies the serialized envelope contract.

## Capabilities

### New Capabilities

- `operation-event-wire-contract`: Defines the canonical payload type metadata used when publishing an `OperationEvent`.

### Modified Capabilities

None.

## Impact

- Affected implementation: `client/src/bus_client.cpp` and operation-event envelope encoding tests.
- Wire behavior: operation events carry the correct existing Protobuf full name; payload bytes, topic, schema, and public API remain unchanged.
- Compatibility: this corrects invalid metadata rather than introducing a new message type or schema version.
