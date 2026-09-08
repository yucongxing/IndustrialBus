# Operation Event Wire Contract Specification

## Purpose

Defines the canonical wire metadata for operation lifecycle events so typed consumers and protocol validators can identify their Protobuf payload consistently.

## Requirements

### Requirement: Operation events use their canonical Protobuf type name

The system SHALL set the `payload_type` of every serialized `OperationEvent` envelope to `industrial_bus.operation.v1.OperationEvent`, matching the full name declared by the OperationEvent Protobuf descriptor.

#### Scenario: Publishing an operation lifecycle update

- **WHEN** a connected client reports a valid operation lifecycle update
- **THEN** the resulting envelope has `payload_type` equal to `industrial_bus.operation.v1.OperationEvent`
- **AND** the envelope payload can be decoded as `industrial_bus.operation.v1.OperationEvent`

### Requirement: Correcting the payload type does not alter routing or schema

The system MUST preserve the existing `system.operation.event` topic, message payload fields, protocol version, and schema version when correcting the OperationEvent payload type metadata.

#### Scenario: Comparing operation event behavior before and after the correction

- **WHEN** an operation event is emitted after this change
- **THEN** its routing topic remains `system.operation.event`
- **AND** no new Protobuf message or schema version is introduced

### Requirement: Operation lifecycle messages use event semantics

The system SHALL publish OperationEvent lifecycle transitions on `system.operation.event` with MessageType `EVENT`, payload role `EVENT_FACT`, and payload type `industrial_bus.operation.v1.OperationEvent`. Non-terminal `CREATED`, `RUNNING`, and `CANCELLING` transitions SHALL use P2; terminal `COMPLETED`, `FAILED`, and `CANCELLED` transitions SHALL use P0.

#### Scenario: Non-terminal operation transition is published

- **WHEN** an operation reports `CREATED`, `RUNNING`, or `CANCELLING`
- **THEN** the Envelope uses `EVENT/P2` on `system.operation.event`
- **AND** its payload is an OperationEvent with the same operation ID and state

#### Scenario: Terminal operation transition is published

- **WHEN** an operation reports `COMPLETED`, `FAILED`, or `CANCELLED`
- **THEN** the Envelope uses `EVENT/P0` on `system.operation.event`
- **AND** its payload is an OperationEvent with the same operation ID and terminal state

### Requirement: Operation progress uses the Progress contract

The system SHALL publish operation progress on `system.operation.progress` with MessageType `PROGRESS`, priority P2, payload role `EVENT_FACT`, and payload type `industrial_bus.operation.v1.ProgressEvent`. The Envelope operation ID and ProgressEvent operation ID MUST match. Reporting progress MUST NOT emit an OperationEvent lifecycle transition.

#### Scenario: OperationGuard reports progress

- **WHEN** an active operation reports a stage, percentage, count, or progress message
- **THEN** the Envelope uses `PROGRESS/P2` on `system.operation.progress`
- **AND** the payload is a ProgressEvent containing the reported fields
- **AND** no lifecycle state transition is implied

#### Scenario: Progress IDs disagree

- **WHEN** an operation Progress Envelope and its ProgressEvent payload contain different operation IDs
- **THEN** the message is rejected as a protocol error before routing
