## Context

The public Topic templates currently carry string views but have no authoritative registry. A private simulated-app header centralizes some dynamic channel strings, while system and operation Topics remain embedded in serializers and client code. Runtime decoding now validates MessageType, payload role, semantic class, priority, and one protocol-owned Progress payload invariant; `CommHubServer` nevertheless handles several control Topics before the common decoder, and `MessageRouter` accepts any other Topic once its routing shape is valid.

The repository builds offline with CMake 3.20+, C++17, and locally installed Protobuf. Generated Protobuf sources live in the build tree, public headers are installed through CMake, and CommHub Core intentionally remains independent of Protobuf parsing. The solution must preserve that boundary and the source compatibility of the existing Topic value types.

## Goals / Non-Goals

**Goals:**

- Make one reviewed catalog input authoritative for typed SDK declarations and runtime validation data.
- Cover literal and segment-parameterized Topics without regex ambiguity or injection through runtime values.
- Apply one deterministic validation pipeline to application and system/control Envelopes before any Hub state mutation.
- Keep ordinary business payloads opaque while retaining explicit validation for protocol-owned cross-field contracts.
- Enforce schema and catalog compatibility offline through the existing build and test workflow.

**Non-Goals:**

- Runtime Topic registration, plugin-supplied catalogs, or catalog negotiation between peers.
- Payload-level business validation, JSON/schema reflection services, or dynamic Protobuf decoding in CommHub.
- Protocol v2 negotiation, cross-node hop forwarding, finite delivery retry, or operation status-query compensation.
- Replacing the existing `EventTopic`, `CommandTopic`, and `RequestTopic` public value types.

## Decisions

### 1. Use a Protobuf text-format manifest as the catalog source

Add a small catalog schema and a checked-in `topics.textproto` manifest under `protocol/catalog/`. A binding records a stable C++ symbol, literal Topic or segmented template, parameter constraints, MessageType, payload role, payload full name, schema major, maximum payload bytes, and optional protocol-owned validator kind.

A repository-owned C++ generator uses Protobuf TextFormat and the current descriptor set to validate and generate outputs. This reuses an existing required dependency, is fully offline, and lets the generator prove that every payload full name exists. YAML or JSON would require a new parser; CMake-only string generation would make structural validation and diagnostics brittle; hand-written parallel C++ tables would recreate the drift this change is intended to remove.

The initial manifest covers all current system control Topics, operation lifecycle/progress Topics, logging Topics, and simulated CMP channel Topics. Command and Request definitions expand into input, success Reply, and standard Error Reply bindings as applicable.

### 2. Generate checked-in typed declarations and immutable runtime data

The generator emits:

- a public generated header containing typed literal Topic declarations and checked factories for parameterized Topics;
- an internal generated catalog table consumed through a small read-only `TopicCatalogView` API;
- a stable catalog fingerprint used by generation and compatibility tests.

Generated outputs are checked in so public API changes are reviewable and source distributions remain usable. A generator `--check` mode writes to a temporary location and compares byte-for-byte with the checked-in files. Output ordering is canonical, so repeated clean generations are deterministic.

The build first compiles the existing Protobuf schema target and the standalone catalog generator, then verifies or regenerates catalog outputs before compiling catalog consumers. The generator links only repository targets and `protobuf::libprotobuf`; it does not add a network or package-manager dependency.

### 3. Match parameterized Topics by validated segments

Parameterized definitions use whole-segment placeholders such as `cmp.channel.{channel_uuid}.simulation.run`. Each placeholder declares a constraint such as UUID. Generated SDK factories validate and escape nothing: a value either satisfies one segment constraint or construction fails.

Runtime lookup tokenizes on `.` and compares literal segments plus typed parameter validators. Literal bindings are checked first. Catalog generation rejects duplicate keys and any pair of patterns that can match the same concrete Topic for the same MessageType and payload role. General regular expressions and multi-segment wildcards are excluded to keep matching deterministic and bounded.

### 4. Keep catalog validation in the protocol/runtime boundary

Introduce a `ProtocolValidator` beside Envelope encoding/decoding. It consumes `ProtocolLimits`, `TopicCatalogView`, supported protocol versions, and the current wall-clock value. `CommHubServer` invokes it for every inbound frame before branching on registration, subscription, heartbeat, disconnect, or routable application Topics. CommHub Core continues receiving already validated `RouteMessage` values and remains Protobuf-independent.

Validation order is fixed:

1. reject an oversized frame before Protobuf parsing;
2. parse the Envelope and validate required fields, lengths, counts, time bounds, hop bounds, and type-specific fields;
3. apply the existing MessageType/payload-role/semantic-class/priority validation and narrowly scoped legacy class fallback;
4. resolve the catalog binding and validate payload type, schema major, and per-binding payload size;
5. invoke an explicitly registered protocol-owned payload validator, if any;
6. only then authenticate or mutate Hub session, subscription, heartbeat, return-route, routing, or QoS state.

Outbound SDK encoding runs the same structural, semantic, and catalog checks that are meaningful before transport, producing immediate local errors for SDK defects. Tests construct a `TopicCatalogView` explicitly rather than weakening the production catalog with arbitrary `test.*` Topics.

### 5. Centralize conservative versioned limits

Add one public `ProtocolLimits` value with injectable overrides and these initial defaults:

- frame: 2 MiB; payload: 1 MiB globally, further restricted per catalog binding;
- protocol identifier/service/instance/idempotency fields: 128 bytes;
- Topic and payload full name: 256 bytes;
- headers: 32 entries, 64-byte keys, 1024-byte values, and 16 KiB aggregate bytes;
- maximum allowed `max_hops`: 8;
- future timestamp skew: 5 minutes; maximum Command/Request deadline horizon: 24 hours;
- supported protocol version range: version 1 for this change.

Exact-boundary values are accepted. Required IDs use the existing protocol-ID character policy rather than imposing UUID-only values. Command/Request require a destination and bounded future deadline; Command additionally requires an idempotency key. Reply requires a causation ID. Types for which these fields have no defined meaning must leave them empty or zero.

These values are centralized defaults rather than scattered literals. Tests can inject smaller limits to exercise every boundary without large allocations.

### 6. Treat reserved headers and protocol-owned payload checks explicitly

The validator owns the `industrial_bus.` namespace. Each reserved key has a value validator and an allowed set of binding contexts. `industrial_bus.message_class` remains valid according to canonical semantics, while unknown reserved keys are rejected. Authentication remains a system registration concern and is allowed only on the corresponding control binding; public application publish APIs still expose no arbitrary header injection.

CommHub does not parse catalog-declared business payloads. The catalog can name a closed enum of protocol-owned validator kinds for system contracts whose Envelope fields must match payload fields. The existing `system.operation.progress` operation-ID equality check moves behind that mechanism instead of remaining a Topic string special case. Arbitrary callbacks or application validators are not loadable into CommHub.

### 7. Return a distinct protocol error before retaining state

Add an internal Hub protocol-error status that maps to the standard `PROTOCOL_ERROR` payload category. Validation failures do not create a session, mutate a subscription, touch heartbeat state, enqueue a destination frame, or install a return route. If enough source metadata is valid to reply safely, CommHub sends a standard `REPLY/ERROR_DETAIL/Error`; otherwise it drops the malformed frame and records the diagnostic.

This distinguishes malformed contracts from application invalid arguments and resource exhaustion. Error replies themselves use catalog-declared bindings and existing P0 semantics.

### 8. Check descriptors, fixtures, catalog evolution, and generation in CTest

Store a released descriptor-set baseline, a catalog baseline, and representative binary Envelope/payload fixtures under `protocol/compat/v1/`. A repository-owned compatibility test uses Protobuf descriptor APIs to detect tag reuse, incompatible type/cardinality changes, missing reservations, and enum value removal or renumbering. It also compares catalog bindings and rejects in-place payload or meaning changes under one Topic/schema major.

Fixture tests parse historical frames using current generated code and verify known values plus unknown-field round trips. Generator tests cover missing descriptors, duplicate/ambiguous patterns, deterministic output, and `--check` freshness. All targets register with CTest and therefore run in the existing GitHub Actions job without a new hosted service or download step.

## Risks / Trade-offs

- [Strict unknown-Topic rejection breaks ad hoc publishers and existing tests] -> Catalog every current production/example Topic first, inject explicit test catalogs, and enable Hub enforcement only after migrations are complete.
- [A checked-in generated API can become stale] -> Make `--check` part of the default test suite and print the exact regeneration command on mismatch.
- [Parameterized patterns can create ambiguous routing contracts] -> Restrict placeholders to whole segments, reject overlaps during generation, and prioritize literals deterministically.
- [Protocol limits reject traffic that was previously accepted] -> Publish centralized defaults, test exact boundaries, keep limits injectable, and report the violated limit in protocol diagnostics.
- [Compatibility tooling can mistake harmless schema evolution for a break] -> Compare descriptor semantics rather than source text and keep explicit positive fixtures for allowed additive changes.
- [Validation duplicates some client-side checks] -> Share the semantic/catalog primitives while retaining Hub validation as the security and correctness boundary.

## Migration Plan

1. Add failing generator/catalog tests and compatibility fixtures without enabling strict Hub rejection.
2. Introduce the catalog schema, manifest, generator, generated outputs, and runtime lookup API; verify the complete current Topic set.
3. Add `ProtocolLimits` and validator unit tests, including exact boundary and side-effect assertions.
4. Migrate system, operation, logging, and simulated-app code to generated declarations/factories; give tests explicit catalog fixtures.
5. Route every CommHub inbound path through the validator, add protocol-error mapping, and remove one-off metadata validation only after equivalent registered validators pass.
6. Enable generated-output and descriptor/catalog compatibility gates in CTest and CI, then run the complete suite and strict OpenSpec validation.

Rollback consists of disabling strict catalog lookup at the server boundary while retaining the additive catalog/generated APIs. No wire schema field is removed by this change.
