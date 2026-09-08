## Why

CommHub currently validates MessageType, payload role, semantic class, and priority, but it still accepts arbitrary Topic/payload-type combinations and lacks the centralized Envelope limits required by the development guide. Topic declarations are also hand-maintained string containers, so SDK code, Hub validation, and compatibility CI do not share one authoritative contract.

## What Changes

- Introduce one declarative Topic Catalog covering current system, operation, logging, and simulated-application traffic, including parameterized channel Topics.
- Validate the catalog for duplicate or ambiguous bindings and generate typed C++ Topic declarations plus the immutable runtime lookup table consumed by the SDK and CommHub.
- Add centralized `ProtocolLimits` and a single pre-routing Envelope validator for identifiers, versions, deadlines/timestamps, hop counts, headers, payload size, semantic metadata, and catalog bindings.
- Reject unknown Topics and mismatched `(topic, MessageType, payload_role, payload_type, schema major)` combinations as protocol errors before routing, queueing, or return-route creation, while keeping business payload bytes opaque to CommHub.
- Preserve explicitly documented compatibility handling for legacy semantic-class metadata without weakening Topic or schema validation.
- Add historical Protobuf fixtures, descriptor/catalog compatibility checks, generated-output freshness checks, and CI coverage for compatible and breaking schema changes.
- Migrate current production and simulated-app Topic definitions to generated catalog declarations. Finite message retry and operation status-query compensation remain outside this change.

## Capabilities

### New Capabilities

- `topic-catalog`: Defines the authoritative static and parameterized Topic bindings and the generated SDK/runtime representations derived from them.
- `envelope-protocol-validation`: Defines centralized protocol limits and catalog-aware validation that every Envelope must pass before Hub routing.
- `protocol-compatibility-ci`: Defines historical schema fixtures and automated compatibility/freshness checks for Protobuf and Topic Catalog evolution.

### Modified Capabilities

None.

## Impact

- Affected APIs: generated typed Topic declarations/factories, catalog lookup types, protocol validation errors, and test-only catalog injection.
- Affected implementation: `protocol`, `api`, `client`, `runtime`, CommHub integration, simulated applications, CMake, and GitHub Actions CI.
- Compatibility: existing typed Topic structures remain source compatible, but an Envelope using an unregistered Topic or invalid binding will now be rejected instead of routed.
- Build constraints: catalog generation and compatibility checks remain offline and repository-owned, using the existing CMake and Protobuf toolchain without downloading dependencies.
