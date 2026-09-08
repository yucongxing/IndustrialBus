## Context

See `proposal.md` for motivation and `specs/operation-event-wire-contract/spec.md` for the wire contract. The client currently attaches a hard-coded type name to a structured `OperationUpdate`; the runtime codec independently serializes that structure as the OperationEvent message declared in `operation.proto`. The routing topic and serialized payload are already correct.

## Goals / Non-Goals

**Goals:**

- Derive the emitted payload type from the authoritative Protobuf definition.
- Cover both client message construction and envelope serialization with focused regression tests.

**Non-Goals:**

- Renaming `system.operation.event`.
- Changing OperationEvent fields, protocol/schema versions, MessageType, or PayloadRole.
- Addressing the broader Topic Catalog and protocol validation gaps.

## Decisions

### Use the Protobuf descriptor as the source of the full type name

`BusClient::reportOperation()` will obtain the canonical name from `industrial_bus::operation::v1::OperationEvent::descriptor()->full_name()` instead of introducing another corrected string literal. The Client target already depends on the Protocol target, so this adds no dependency or runtime subsystem.

Alternative considered: replace only the bad literal. That is smaller but continues to duplicate schema-owned metadata and permits the same typo to recur if the message package changes.

### Test at the client and envelope boundaries

A client test will verify that `reportOperation()` constructs an outbound operation message whose payload type matches the descriptor. An envelope codec test will verify that the wire envelope preserves that canonical name and contains a decodable OperationEvent payload while retaining the existing topic.

Alternative considered: test only the corrected source literal. That would not protect the handoff between client construction and envelope serialization.

## Risks / Trade-offs

- [Risk] Descriptor access introduces a dependency on full Protobuf reflection. -> The project already links `protobuf::libprotobuf` and generated messages use the full runtime; compilation of the descriptor-based test will verify this assumption.
- [Risk] Expanding the patch into general metadata validation could obscure this targeted compatibility fix. -> Keep validation and Topic Catalog work explicitly out of scope.

## Migration Plan

No data migration is required. Deploy the corrected Client library; rollback consists of reverting the client metadata change and its regression tests. Existing consumers that already expect the canonical Protobuf full name begin matching operation events after upgrade.
