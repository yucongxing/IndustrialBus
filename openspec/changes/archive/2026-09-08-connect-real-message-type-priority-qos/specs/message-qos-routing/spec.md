## Purpose

Defines how application message semantics become canonical wire types and priorities, and how CommHub schedules bounded per-connection traffic without allowing a slow consumer to interfere with healthy peers.

## ADDED Requirements

### Requirement: Message types remain distinct across the runtime boundary

The system SHALL preserve each supported Envelope MessageType as a distinct value through outbound construction, wire encoding, Hub routing, wire decoding, and inbound delivery. Operation is a payload domain and MUST NOT replace the Envelope MessageType. An unspecified or unsupported MessageType MUST be rejected before routing.

#### Scenario: Progress retains its wire type

- **WHEN** a publisher sends a progress-class message
- **THEN** the Envelope carries MessageType `PROGRESS`
- **AND** the Hub and receiving runtime continue to classify it as Progress

#### Scenario: Log retains its wire type

- **WHEN** a publisher sends a log-class message
- **THEN** the Envelope carries MessageType `LOG`
- **AND** the receiving runtime does not expose it as an ordinary Event

#### Scenario: Asynchronous error remains distinct from reply error

- **WHEN** an asynchronous protocol error is sent without corresponding to an in-flight Command or Request
- **THEN** its MessageType is `ERROR`
- **AND** an Error that rejects a Command or Request continues to use `REPLY` with the error-detail payload role

#### Scenario: Unspecified message type is rejected

- **WHEN** the Hub receives a routable Envelope with MessageType `MESSAGE_TYPE_UNSPECIFIED`
- **THEN** it rejects the Envelope as a protocol error without enqueueing it

### Requirement: Semantic classes determine canonical priority

The system SHALL assign P0 to Reply, Accepted, asynchronous Error, reply Error, and terminal-state messages; P1 to Command, Request, Heartbeat, and service/system-state messages; P2 to ordinary Event, Progress, and warning or error Log messages; and P3 to informational and debug Log messages. SDK-controlled Command, Request, Reply, Accepted, Error, Heartbeat, System, and Operation paths MUST derive this priority rather than accepting an arbitrary caller override.

#### Scenario: Directed request classes use P1

- **WHEN** the SDK emits a Command or Request
- **THEN** the Envelope priority is P1

#### Scenario: Completion and replies use P0

- **WHEN** the SDK emits a Reply, Accepted acknowledgement, error reply, asynchronous Error, or terminal-state Event
- **THEN** the Envelope priority is P0

#### Scenario: Event and progress traffic use normal priority

- **WHEN** the SDK emits an ordinary Event, Progress, or warning or error Log
- **THEN** the Envelope priority is P2

#### Scenario: Low-severity logs use best-effort priority

- **WHEN** the SDK emits an informational or debug Log
- **THEN** the Envelope priority is P3

### Requirement: Publisher semantics are explicit and backward compatible

The typed publish API SHALL treat existing event publications as ordinary `EVENT/P2` traffic by default and SHALL provide an explicit semantic classification for Progress, final-state Event, and Log publications. Existing source code that publishes an Event without the new classification MUST remain source compatible.

#### Scenario: Existing event publisher is rebuilt

- **WHEN** an existing caller uses a two-field Event topic and default publish options
- **THEN** the caller continues to compile
- **AND** the resulting Envelope is classified as `EVENT/P2`

#### Scenario: Publisher declares progress semantics

- **WHEN** a publisher selects the Progress semantic class for a payload and supplies an operation ID
- **THEN** the resulting Envelope is classified as `PROGRESS/P2`

#### Scenario: Publisher declares log severity

- **WHEN** a publisher selects error, warning, informational, or debug Log semantics
- **THEN** the resulting Envelope uses MessageType `LOG` and the priority defined for that severity

### Requirement: Wire priority is validated before scheduling

The Hub MUST reject priorities outside P0 through P3 and MUST reject MessageType/priority combinations that cannot result from the canonical semantic mapping. `EVENT/P0` MUST remain valid for terminal-state events, while ordinary Event publication defaults to P2.

#### Scenario: Priority is outside the protocol range

- **WHEN** the Hub receives an Envelope whose priority is greater than P3
- **THEN** it rejects the Envelope as a protocol error without enqueueing it

#### Scenario: Progress claims critical priority

- **WHEN** the Hub receives a `PROGRESS` Envelope with P0 priority
- **THEN** it rejects the Envelope as a protocol error without enqueueing it

#### Scenario: Legacy sender omits semantic classification

- **WHEN** a valid Envelope from a legacy sender has no SDK semantic-class metadata
- **THEN** the Hub derives a safe default class and effective priority from its MessageType and payload role
- **AND** it does not reject the message solely because the new metadata is absent

### Requirement: QoS queues honor priority and FIFO order

Each destination connection SHALL have a bounded QoS queue that selects P0 before P1, P1 before P2, and P2 before P3 while preserving FIFO order among non-coalesced messages at the same priority.

#### Scenario: Multiple priorities are queued

- **WHEN** messages at P3, P1, P2, and P0 are pending for one destination
- **THEN** they are dequeued in P0, P1, P2, P3 order

#### Scenario: Same-priority messages are queued

- **WHEN** two non-coalesced messages at the same priority are accepted in order
- **THEN** they are dequeued in that order

### Requirement: Progress is coalesced by operation

Before applying overflow behavior, a destination queue SHALL replace an older queued Progress message with the newest Progress for the same operation. Progress for different operations MUST remain independent, and lifecycle Events MUST NOT be coalesced as Progress.

#### Scenario: New progress supersedes queued progress

- **WHEN** two Progress messages for the same operation are enqueued before the first is delivered
- **THEN** only the newest Progress remains queued for that operation
- **AND** the progress-coalesced metric increments

#### Scenario: Independent operations report progress

- **WHEN** Progress messages for two different operation IDs are enqueued
- **THEN** neither message replaces the other

#### Scenario: Terminal state follows queued progress

- **WHEN** a terminal-state Event arrives while Progress for the same operation is queued
- **THEN** the terminal Event retains P0 priority and is not coalesced away as Progress

### Requirement: Queue overflow follows semantic value

When a destination queue reaches its bound, the scheduler SHALL first evict eligible lower-value queued traffic in the order Debug Log, informational Log, Progress, ordinary Event, and warning Log when doing so admits a more valuable incoming message. If no eligible item can be evicted, P2 and P3 input SHALL be dropped and counted, P1 input SHALL be rejected with resource exhaustion, and P0 exhaustion SHALL identify the destination as a slow consumer and require its disconnection.

#### Scenario: Higher-value input evicts best-effort traffic

- **WHEN** a full destination queue contains eligible P3 traffic and a P1 message arrives
- **THEN** the scheduler evicts eligible lower-value traffic and accepts the P1 message

#### Scenario: Directed P1 traffic cannot be admitted

- **WHEN** a full destination queue has no eligible lower-value entry and a Command or Request arrives
- **THEN** the message is rejected with resource exhaustion
- **AND** any return route created for that rejected message is removed

#### Scenario: Droppable traffic cannot be admitted

- **WHEN** a full destination queue has no less valuable entry and a P2 or P3 message arrives
- **THEN** the incoming message is dropped without disconnecting the destination
- **AND** the dropped metric increments

#### Scenario: Critical traffic cannot be admitted

- **WHEN** a full destination queue has no eligible lower-value entry and a P0 message arrives
- **THEN** the destination is classified as a slow consumer and disconnected
- **AND** the critical-overflow metric increments

### Requirement: Slow consumers are isolated per destination

Routing and queue outcomes for one destination SHALL NOT prevent the same broadcast message from being evaluated and enqueued independently for every other destination. Disconnecting a slow subscriber MUST remove only that subscriber's session state and queue.

#### Scenario: One broadcast subscriber has critical overflow

- **WHEN** a P0 broadcast targets one exhausted subscriber and one healthy subscriber
- **THEN** the exhausted subscriber is disconnected
- **AND** the healthy subscriber still receives or queues the message

#### Scenario: One subscriber drops normal traffic

- **WHEN** a P2 broadcast is dropped for one full subscriber queue
- **THEN** the Hub continues delivery evaluation for every other subscriber

### Requirement: QoS outcomes are observable

The scheduler SHALL maintain per-destination counters for accepted, dequeued, coalesced, dropped, rejected, and critical-overflow outcomes, together with the queue high-water mark.

#### Scenario: Queue activity is inspected

- **WHEN** a destination has accepted, coalesced, dropped, rejected, or dequeued traffic
- **THEN** its QoS statistics reflect each corresponding outcome and its maximum observed depth
