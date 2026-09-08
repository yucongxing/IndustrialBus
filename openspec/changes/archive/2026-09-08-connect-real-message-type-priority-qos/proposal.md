## Why

The protocol defines distinct message types and four QoS priorities, but the client runtime collapses several types into Event and never writes the Envelope priority. As a result, normal events, progress, and logs are routed as critical P0 traffic, progress cannot be coalesced by the live Hub scheduler, and one slow subscriber can interfere with delivery to others.

## What Changes

- Preserve the real protocol MessageType across the client/runtime boundary instead of representing Progress, Log, Error, Heartbeat, and System as Event or Reply aliases.
- Derive and write canonical priorities from message semantics: P0 for replies, acknowledgements, protocol errors, and terminal states; P1 for commands, requests, heartbeat, and service state; P2 for ordinary events, progress, and warning/error logs; P3 for informational and debug logs.
- Provide a source-compatible way for publishers and topic declarations to identify Progress, final-state, and Log semantics while keeping ordinary Event as the default.
- **BREAKING WIRE BEHAVIOR**: Publish operation lifecycle transitions as OperationEvent messages and operation progress as ProgressEvent messages; consumers that treated progress as an OperationEvent must subscribe to the progress contract instead. Coalesce queued Progress by operation.
- Apply bounded per-connection overflow behavior appropriate to each class, including explicit P1 rejection, droppable P2/P3 traffic, and P0 slow-consumer isolation.
- Ensure failure or disconnection of one broadcast subscriber does not prevent independent subscribers from receiving or queueing the same message.
- Consolidate the production QoS policy so its classification, ordering, coalescing, overflow, and metrics do not diverge from the reusable queue policy.
- Exclude critical-message retry and operation-status recovery; those reliability mechanisms will be specified in a separate change.

## Capabilities

### New Capabilities

- `message-qos-routing`: Defines lossless MessageType propagation, semantic priority assignment, progress coalescing, bounded overflow behavior, and per-subscriber slow-consumer isolation.

### Modified Capabilities

- `operation-event-wire-contract`: Distinguishes OperationEvent lifecycle messages from ProgressEvent updates and assigns terminal versus non-terminal QoS priority without changing the established OperationEvent topic or schema versions.

## Impact

- Public API metadata in `api/include/industrial_bus/`, including QoS, publish options, and typed topic declarations.
- Client/runtime message models and Envelope encoding/decoding in `client/` and `runtime/`.
- Operation and progress helpers in `integration/` and their Protobuf payload selection.
- CommHub routing, per-connection QoS queues, overflow metrics, and broadcast isolation in `commhub/`.
- Simulated application topic metadata and log/progress publishing.
- Unit, codec, client-runtime, CommHub, and end-to-end tests for the complete semantic-to-wire-to-queue path.
- No Protobuf schema-version or protocol-version bump is required because MessageType and priority already exist in the Envelope schema.
