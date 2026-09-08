# Protocol Compatibility CI Specification

## Purpose

Defines offline compatibility gates that keep Protobuf schemas, historical wire fixtures, Topic Catalog bindings, and generated catalog representations safe across releases.

## Requirements

### Requirement: Protobuf evolution is checked against a historical baseline

CI SHALL compare current Protobuf descriptors with a checked-in released baseline and reject field-number reuse, incompatible field-type changes, removal without reservation, enum renumbering, or removal of published enum values.

#### Scenario: A compatible field is added

- **WHEN** a new field with a new number and valid default semantics is appended
- **THEN** the compatibility check accepts the schema

#### Scenario: A published field number is reused

- **WHEN** a schema assigns an existing or reserved field number to a different meaning
- **THEN** the compatibility check fails and identifies the message and field number

### Requirement: Historical wire fixtures remain decodable

The repository SHALL keep representative binary fixtures produced by released protocol schemas and SHALL decode them with the current generated code while preserving all fields known to the historical schema.

#### Scenario: Current code reads a historical fixture

- **WHEN** compatibility tests parse each checked-in historical Envelope and payload fixture
- **THEN** parsing succeeds and the expected historical field values remain intact

#### Scenario: Current code receives historical unknown fields

- **WHEN** a fixture is decoded and re-encoded through a reader that does not interpret a newer field
- **THEN** Protobuf unknown-field compatibility is preserved

### Requirement: Topic Catalog evolution preserves published bindings

CI SHALL compare the current Topic Catalog with a checked-in released baseline. Within one concrete Topic pattern and schema major, an existing direction key MUST NOT change its payload full name or meaning; an incompatible contract MUST use a new schema major and a new versioned Topic or namespace.

#### Scenario: A compatible payload schema evolves

- **WHEN** compatible Protobuf fields are added while the Topic binding and schema major remain unchanged
- **THEN** the catalog compatibility check succeeds

#### Scenario: A payload binding changes in place

- **WHEN** an existing Topic direction is rebound to an incompatible payload type under the same schema major
- **THEN** CI fails and identifies the old and new binding

### Requirement: Catalog generation is deterministic and current

The same catalog and Protobuf descriptor inputs SHALL produce byte-identical generated catalog outputs. CI MUST regenerate or verify those outputs and fail on nondeterminism, stale checked-in representations, missing payload descriptors, duplicate keys, or ambiguous Topic patterns.

#### Scenario: Generated outputs are current

- **WHEN** CI generates catalog outputs from an unchanged catalog and descriptor set
- **THEN** they match the expected outputs without a working-tree difference

#### Scenario: Catalog generation is stale

- **WHEN** a catalog input changes without its required generated representation or baseline update
- **THEN** CI fails with regeneration guidance

### Requirement: Compatibility checks run offline in the standard suite

All schema, fixture, catalog, and generation compatibility checks SHALL run through the repository's CMake/CTest and CI workflow using only locally installed Protobuf and repository-owned tools.

#### Scenario: CI runs without dependency downloads

- **WHEN** the standard CI job configures, builds, and tests the repository with its declared system packages
- **THEN** every compatibility check runs without downloading an additional generator or validation service
