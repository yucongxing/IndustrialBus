# Topic Catalog Specification

## Purpose

Defines one authoritative catalog from which applications obtain typed Topics and CommHub obtains the matching runtime contracts for every routable message direction.

## Requirements

### Requirement: The catalog is the authoritative Topic contract

The system SHALL define every production-routable Topic in one catalog. Each binding MUST identify a Topic or parameterized Topic pattern, MessageType, payload role, Protobuf full type name, schema major, and maximum payload size, and the catalog MUST reject duplicate binding keys.

#### Scenario: A declared Topic is resolved

- **WHEN** a catalog consumer looks up a declared `(topic, MessageType, payload role)` key
- **THEN** it receives exactly one binding containing the payload type, schema major, and payload-size limit

#### Scenario: A duplicate binding is declared

- **WHEN** two catalog entries declare the same effective binding key
- **THEN** catalog validation fails before the product is built or packaged

### Requirement: Bidirectional Topics declare every legal payload role

Command and Request Topics SHALL declare their input and successful Reply bindings. A Topic that permits rejection MUST also declare the standard `REPLY/ERROR_DETAIL/industrial_bus.system.v1.Error` binding; no undeclared response role or payload type is implicitly accepted.

#### Scenario: A Command contract is generated

- **WHEN** the catalog declares a Command Topic
- **THEN** its Command input and CommandAck Reply bindings are both present
- **AND** its standard Error Reply binding is present when rejection is allowed

#### Scenario: An undeclared response is considered

- **WHEN** a response uses a payload role or payload type absent from its Topic contract
- **THEN** catalog lookup does not return a compatible binding

### Requirement: Parameterized Topics resolve safely

The catalog SHALL support parameterized Topic patterns needed for runtime identifiers such as channel UUIDs. Parameter values MUST satisfy their declared segment constraints, literal Topics MUST take precedence over patterns, and patterns that can ambiguously match the same Topic and direction MUST be rejected during catalog validation.

#### Scenario: A valid channel Topic is instantiated

- **WHEN** an application supplies a valid channel UUID to a generated channel Topic factory
- **THEN** it receives the matching typed Topic with the UUID substituted into exactly one segment

#### Scenario: An invalid parameter is supplied

- **WHEN** a Topic parameter contains an invalid UUID, separator, wildcard, or empty value
- **THEN** Topic construction fails without producing a routable Topic

#### Scenario: Catalog patterns overlap

- **WHEN** two parameterized entries could match the same concrete Topic and direction
- **THEN** catalog validation reports the ambiguity and generation fails

### Requirement: Generated SDK and runtime views remain identical

One catalog input SHALL generate both the typed C++ Topic declarations used by publishers/subscribers and the immutable lookup data used by protocol validation. Generation MUST be deterministic, and all current system, operation, logging, and simulated-application Topics MUST be represented.

#### Scenario: Catalog outputs are generated

- **WHEN** the catalog generator processes a valid catalog
- **THEN** the SDK declarations and runtime lookup contain the same binding keys and metadata

#### Scenario: A referenced Protobuf type does not exist

- **WHEN** a catalog binding names a Protobuf full type absent from the current descriptor set
- **THEN** generation fails with the offending Topic and payload type

### Requirement: Existing typed Topic APIs remain source compatible

Existing `EventTopic`, `CommandTopic`, and `RequestTopic` value types SHALL remain source compatible. Production and example call sites SHALL migrate to generated declarations or factories, and a custom Topic MUST be added to the catalog before CommHub will route it.

#### Scenario: Existing typed Topic code is rebuilt

- **WHEN** existing code constructs one of the public typed Topic value types directly
- **THEN** that code continues to compile
- **AND** runtime routing succeeds only if the resulting binding exists in the catalog
