## Context

See `proposal.md` for motivation. The Envelope already carries MessageType and a numeric priority, but the client adapter model exposes only Event, Command, Request, Reply, and an Operation pseudo-kind. Encoding omits priority, decoding collapses several protocol types, and the Hub then reads protobuf's numeric default as P0. The public `MessageClass` and its default-priority table are not connected to the runtime. CommHub has a separate per-priority scheduler while the reusable bounded queue implements richer semantic eviction but is unused in production.

The Hub must continue treating payload bytes as opaque. This matters for Log severity: INFO and DEBUG share MessageType `LOG` and P3, so the scheduler cannot distinguish their eviction order by MessageType and priority alone. Existing callers also need to keep compiling when an ordinary Event topic has only its current name and payload type.

## Goals / Non-Goals

**Goals:**

- Establish one semantic record containing MessageClass, exact MessageType, and effective QosPriority from SDK publication through Hub scheduling.
- Keep protocol-generated types behind the runtime codec boundary and use project-owned enums in the public/client layers.
- Make the existing semantic queue policy the production source of truth and keep per-destination state independent.
- Preserve source compatibility for existing ordinary Event publishers and accept legacy wire messages that lack the new semantic metadata.

**Non-Goals:**

- Retrying P0 delivery or persisting outbound messages.
- Implementing `operation.get_status` recovery or any other compensation workflow.
- Implementing the full Topic Catalog or parsing application payloads in CommHub.
- Changing Envelope protocol version, business schema version, or existing protobuf field numbers.

## Decisions

### 1. Represent protocol MessageType exactly and keep Operation as a payload concern

Replace the lossy client `MessageKind` domain with a project-owned MessageType whose supported values mirror the protocol enum. Outbound and inbound runtime messages carry this exact type and a QosPriority. The codec performs exhaustive enum conversion and rejects unspecified or unknown values rather than mapping them to Event or System.

Operation-specific dispatch will use topic, payload type, and payload role rather than an `Operation` pseudo-message type. The normal subscription path will accept Event, Progress, and Log publications while directed handlers remain limited to Command and Request and pending completions remain Reply-only.

Using the generated protobuf enum directly in the public API was rejected because it would couple API consumers and mock adapters to generated protocol headers.

### 2. Use MessageClass as the semantic source for type and priority

Add a small canonical mapping that produces `{message_type, priority}` from MessageClass for publishable traffic. SDK-owned paths construct semantics internally:

| Semantic class | MessageType | Priority |
|---|---|---|
| Reply, Accepted, reply Error | REPLY | P0 |
| Asynchronous Error | ERROR | P0 |
| FinalState | EVENT | P0 |
| Command, Request | COMMAND, REQUEST | P1 |
| Heartbeat, ServiceState | HEARTBEAT, SYSTEM | P1 |
| Event, Progress | EVENT, PROGRESS | P2 |
| ErrorLog, WarnLog | LOG | P2 |
| InfoLog, DebugLog | LOG | P3 |

Add `ErrorLog` to MessageClass so logging an error cannot be confused with the protocol `ERROR` type. EventTopic gains an optional/default semantic class after its existing two fields, and PublishOptions can refine a topic's Log severity for an individual record. Only Event, Progress, FinalState, and Log classes are legal through the generic publish path; callers cannot label a publication as Command, Reply, or protocol Error.

This preserves aggregate initialization of existing two-field EventTopic values and makes their default behavior `EVENT/P2`. Exposing an arbitrary numeric priority override was rejected because it would let applications bypass the semantic policy.

### 3. Carry fine-grained class in an SDK-reserved Envelope header

The encoder writes the canonical numeric priority and an SDK-reserved `industrial_bus.message_class` header. The header is needed only for distinctions that MessageType and priority cannot express, such as InfoLog versus DebugLog and FinalState versus another Event. The API will not expose direct mutation of this reserved header.

The Hub validates header/type/priority combinations before scheduling. Messages from older clients without the header remain acceptable: the Hub derives a conservative class and canonical effective priority from MessageType and payload role. In particular, legacy Command and Request traffic becomes P1, Reply becomes P0, and unclassified Event becomes P2. A present but invalid or inconsistent reserved value is a protocol error.

Adding a new protobuf Envelope field was rejected because the existing headers provide a compatible extension point and avoid a protocol schema edit. Parsing LogEvent in the Hub was rejected because it violates payload opacity.

### 4. Separate Operation lifecycle and progress serialization

`reportOperation()` continues to serialize OperationEvent on `system.operation.event`; it chooses FinalState/P0 for Completed, Failed, and Cancelled and Event/P2 for Created, Running, and Cancelling. It never infers Progress from optional stage fields.

Add a dedicated BusClient progress path that serializes ProgressEvent on `system.operation.progress` as `PROGRESS/P2`, copies the operation ID into the Envelope, and validates that the payload and Envelope IDs agree. OperationGuard and ProgressThrottle use this path for progress reports. Existing OperationUpdate APIs can remain available for lifecycle state so this wire correction does not require changing the OperationEvent schema.

Keeping Progress as OperationEvent was rejected because the protocol already defines ProgressEvent and the Hub coalesces only real PROGRESS messages. Reusing `system.operation.event` was rejected because a distinct progress topic gives subscribers and the future Topic Catalog an unambiguous binding.

### 5. Use one bounded semantic queue per destination

Keep QoSScheduler as the CommHub-facing owner of per-session queues, but delegate queue ordering, coalescing, eviction, and base statistics to the reusable bounded priority queue. RouteMessage is enqueued with QueueMetadata derived from the validated semantic class; Progress uses operation ID as its coalescing key.

Use a total per-connection bound rather than four independently full queues. On a full queue, a more valuable incoming item can evict eligible traffic in semantic discard order. QoSScheduler maps the queue result into Hub behavior: droppable P2/P3 input is a successful drop, non-admittable P1 is resource exhaustion, and non-admittable P0 marks the target as a slow consumer.

Retaining two independently implemented policies was rejected because their metrics, eviction order, and overflow outcomes have already diverged. Retaining only per-priority bounds was rejected because it prevents high-value traffic from reclaiming space occupied by disposable traffic.

### 6. Make broadcast enqueue and transport drain target-local

Hub dispatch evaluates every broadcast target even if an earlier target drops traffic or reaches critical overflow. A critical-overflow target is disconnected and removed, but healthy targets stay in the delivery set. Directed P1 rejection still removes any newly created return route and produces resource exhaustion for the origin.

CommHubServer drains each target independently. A transport send failure stops and disconnects only that target, then drain continues with the remaining targets. This change does not retry the popped frame.

Returning immediately on the first failed broadcast target was rejected because it couples otherwise independent connection queues and violates slow-consumer isolation.

### 7. Verify the semantic-to-wire-to-queue path with TDD

Start with codec/client tests that fail because MessageType is collapsed and priority is absent. Add scheduler tests for ordering, same-operation coalescing, semantic eviction, all overflow outcomes, and statistics. Add Hub tests with two subscribers to prove one slow or failed target cannot suppress delivery to the other. End-to-end tests inspect raw Envelopes for representative Event, Progress, Log, Command, Reply, and Operation terminal messages.

## Risks / Trade-offs

- [Progress changes payload type and topic] -> Mark the wire behavior as breaking, update built-in helpers and examples together, and document that consumers must subscribe to `system.operation.progress` as ProgressEvent.
- [Legacy messages lack MessageClass metadata] -> Apply documented conservative fallback mapping only when the reserved header is absent; reject malformed new metadata.
- [A non-SDK client can mislabel opaque Log payload severity] -> Validate all metadata combinations and reserve the header name; full payload/topic authorization remains a future Topic Catalog responsibility.
- [Replacing per-priority capacities changes queue pressure] -> Preserve approximately the existing default aggregate capacity, expose one explicit per-connection capacity, and cover boundary behavior with deterministic tests.
- [A failed synchronous send has already popped its frame] -> Disconnect only that consumer and record the loss; retry is deliberately deferred.

## Migration Plan

1. Add failing compatibility and behavior tests before changing production paths.
2. Introduce exact runtime types, semantic mapping, reserved-header encoding, and legacy decode fallback while keeping old EventTopic construction valid.
3. Migrate Operation/Progress and simulated Progress/Log/final-state publishers to explicit semantics.
4. Switch QoSScheduler to the shared bounded queue policy and add target-local dispatch/drain handling.
5. Run strict OpenSpec validation plus focused and full test suites. Mixed-version tests must cover a new Hub receiving legacy header-less messages and a new client sending MessageType/priority values understood by the current protocol.

Rollback requires reverting the runtime and publisher changes together. No stored data or protobuf migration is required, but consumers migrated to `system.operation.progress` must be rolled back with producers.
