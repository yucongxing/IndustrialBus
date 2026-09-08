## Purpose

Defines the centralized limits and catalog-aware checks that protect CommHub from malformed, oversized, unknown, or contract-inconsistent Envelopes before routing state is changed.

## ADDED Requirements

### Requirement: Central protocol limits apply to every inbound Envelope

The system SHALL define one `ProtocolLimits` policy covering frame and payload bytes, identifier and Topic lengths, payload-type length, header count and sizes, hop bounds, timestamp skew, and deadline horizon. Every client-originated Envelope, including registration, subscription, heartbeat, and disconnect control messages, MUST pass these limits before topic-specific handling or routing.

#### Scenario: A field is exactly at its limit

- **WHEN** an otherwise valid Envelope contains a field or collection exactly at the configured limit
- **THEN** validation accepts that field or collection

#### Scenario: A frame exceeds a limit

- **WHEN** an Envelope exceeds any configured frame, payload, string, header, hop, or time limit
- **THEN** it is rejected as a protocol error before topic-specific handling

### Requirement: Envelope structure and cross-field semantics are validated

The validator SHALL enforce required protocol identifiers, supported protocol versions, valid timestamps and deadlines, `hop_count <= max_hops`, MessageType/payload-role/semantic-class/priority consistency, and type-specific rules for destination, idempotency key, deadline, causation ID, and operation ID.

#### Scenario: A valid Command is checked

- **WHEN** a Command has valid identifiers, a destination, a non-empty idempotency key, a future bounded deadline, and canonical semantic metadata
- **THEN** structural validation succeeds

#### Scenario: Type-specific metadata is inconsistent

- **WHEN** a non-Command carries an idempotency key, a Reply lacks its request causation ID, or a hop count exceeds its maximum
- **THEN** validation rejects the Envelope as a protocol error

#### Scenario: The protocol version is unsupported

- **WHEN** an Envelope declares a protocol version outside the receiver's supported range
- **THEN** it is rejected before catalog lookup or routing

### Requirement: Topic bindings are validated before routing

After structural validation, the Hub MUST resolve the Envelope through the Topic Catalog and require an exact match for concrete Topic, MessageType, payload role, Protobuf full payload type, and schema major. The payload MUST satisfy both the global and binding-specific size limits.

#### Scenario: A catalog binding matches

- **WHEN** all Envelope binding fields match one catalog entry and its payload is within bounds
- **THEN** catalog validation succeeds without deserializing the application payload

#### Scenario: A Topic is unknown

- **WHEN** an Envelope names a Topic that matches no catalog literal or parameterized pattern
- **THEN** it is rejected as a protocol error

#### Scenario: A binding field disagrees

- **WHEN** a known Topic uses an undeclared MessageType, payload role, payload type, or schema major
- **THEN** it is rejected as a protocol error

### Requirement: Rejected Envelopes have no routing side effects

Protocol validation MUST complete before a destination queue entry, request return route, subscription mutation, service registration, or heartbeat update is created. When a reply is possible, CommHub SHALL return a standard protocol Error without retaining state for the rejected Envelope.

#### Scenario: An invalid Request reaches CommHub

- **WHEN** a Request fails structural or catalog validation
- **THEN** no return route or destination queue entry survives
- **AND** the sender receives a protocol Error Reply when its connection permits a reply

#### Scenario: An invalid registration reaches CommHub

- **WHEN** a registration Envelope fails protocol validation
- **THEN** no session or service entry is installed

### Requirement: Reserved headers are controlled by protocol policy

Header keys with the `industrial_bus.` prefix SHALL be limited to registered protocol meanings, valid values, and permitted message bindings. Application-controlled APIs MUST NOT allow arbitrary reserved-header injection.

#### Scenario: A valid semantic-class header is received

- **WHEN** `industrial_bus.message_class` contains the canonical class for the Envelope binding
- **THEN** header validation accepts it

#### Scenario: An unknown reserved header is received

- **WHEN** an Envelope contains an unregistered `industrial_bus.` header
- **THEN** validation rejects the Envelope as a protocol error

### Requirement: Legacy semantic fallback remains narrowly compatible

A valid legacy Envelope that omits `industrial_bus.message_class` SHALL retain the documented safe semantic fallback. The fallback MUST NOT bypass Topic Catalog, schema-major, payload-type, payload-size, or other `ProtocolLimits` validation.

#### Scenario: A known legacy Event is received

- **WHEN** a header-less legacy Event otherwise matches a catalog binding
- **THEN** the validator derives its safe semantic class and effective priority
- **AND** continues normal catalog validation

#### Scenario: An unknown legacy Topic is received

- **WHEN** a header-less Envelope uses an unknown Topic
- **THEN** it is rejected despite semantic fallback being available

### Requirement: Business payloads remain opaque to CommHub

Catalog validation SHALL use Envelope metadata and payload byte length without decoding application payloads. Only protocol-owned payloads with an explicit cross-field contract MAY use a registered protocol validator, including the existing operation Progress ID equality rule.

#### Scenario: An application Event is validated

- **WHEN** an application Event matches its catalog metadata and size contract
- **THEN** CommHub routes it without parsing its payload bytes

#### Scenario: Protocol-owned Progress IDs disagree

- **WHEN** a canonical operation Progress payload contains an operation ID different from its Envelope
- **THEN** its registered protocol validator rejects it before routing
