## Purpose

Defines the canonical wire metadata for operation lifecycle events so typed consumers and protocol validators can identify their Protobuf payload consistently.

## ADDED Requirements

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
