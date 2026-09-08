## 1. Establish Failing Contract Tests

- [x] 1.1 Add catalog parser/generator tests for valid literal and parameterized bindings, duplicate keys, overlapping patterns, invalid parameters, and missing payload descriptors; verify the new catalog test target builds and fails before the generator implementation exists.
- [x] 1.2 Add deterministic-generation and stale-output tests covering the public declarations, runtime lookup table, and catalog fingerprint; verify `ctest` reports the expected failures against absent or intentionally stale generated outputs.
- [x] 1.3 Add `ProtocolLimits` boundary tests for frame/payload sizes, identifier/topic/type lengths, header count/key/value/aggregate sizes, hop bounds, protocol versions, timestamp skew, and deadline horizon; verify exact-limit cases are accepted and one-over-limit cases initially fail.
- [x] 1.4 Add Envelope cross-field tests for destination, idempotency key, deadline, causation ID, operation ID, semantic class, payload role, priority, and reserved headers; verify invalid type-specific combinations are represented by failing tests.
- [x] 1.5 Add catalog-binding tests for unknown Topics and mismatched MessageType, payload role, payload type, schema major, and per-binding payload size, including legacy header-less messages; verify all mismatch cases initially fail without decoding business payloads.
- [x] 1.6 Add CommHub integration tests proving malformed registration, subscription, heartbeat, disconnect, Request, and application messages create no session, subscription, heartbeat, return-route, queue, or QoS side effects; verify the tests expose every current pre-validation path.
- [x] 1.7 Add failing compatibility-test cases for permitted additive schema/catalog evolution and forbidden field reuse, incompatible type/cardinality changes, unreserved removal, enum renumbering/removal, and in-place catalog rebinding; verify diagnostics name the affected symbol and field or binding.

## 2. Build the Topic Catalog and Generator

- [x] 2.1 Add the catalog Protobuf schema and `protocol/catalog/topics.textproto` manifest with literal and whole-segment parameter bindings for all current system, operation, logging, and simulated-application Topics; verify the manifest enumerates every existing production/example Topic found by the catalog coverage test.
- [x] 2.2 Implement catalog loading and validation for required metadata, stable C++ symbols, descriptor existence, duplicate effective keys, parameter constraints, and ambiguous patterns; verify the parser/generator tests from task 1.1 pass with actionable diagnostics.
- [x] 2.3 Implement deterministic generation of the source-compatible public typed Topic declarations/factories, immutable runtime lookup table, and stable fingerprint; verify two clean generations are byte-identical and task 1.2 passes.
- [x] 2.4 Implement exact-first segmented runtime lookup and checked parameter factories for UUID, empty, separator, and wildcard rejection; verify literal precedence and valid/invalid channel Topic tests pass.
- [x] 2.5 Integrate the catalog schema, standalone generator, generated outputs, install rules, and `--check` mode into CMake using only repository and Protobuf targets; verify a clean offline configure/build succeeds and stale generated files fail with a regeneration command.
- [x] 2.6 Migrate system, operation, logging, and simulated-application call sites to generated Topic declarations/factories while retaining `EventTopic`, `CommandTopic`, and `RequestTopic` construction compatibility; verify all migrated targets compile and no duplicate hand-maintained production Topic constants remain.

## 3. Implement Central Protocol Validation

- [x] 3.1 Add the injectable public `ProtocolLimits` policy with the versioned defaults from `design.md`; verify unit tests assert every default and can substitute small test limits without large allocations.
- [x] 3.2 Implement frame preflight and Envelope structural validation for parsing, required fields, string/count/aggregate limits, supported versions, timestamps/deadlines, and hop bounds; verify tasks 1.3 and the corresponding malformed-frame tests pass.
- [x] 3.3 Implement MessageType-specific destination, idempotency, deadline, causation, and operation metadata rules, reusing the existing semantic-class/payload-role/priority logic; verify task 1.4 passes for all valid and invalid message families.
- [x] 3.4 Implement reserved `industrial_bus.` header registration, context/value validation, and the narrow legacy semantic-class fallback; verify unknown reserved headers fail and header-less known legacy bindings pass without bypassing any other check.
- [x] 3.5 Integrate `TopicCatalogView` lookup into `ProtocolValidator` and enforce exact Topic, MessageType, payload role, payload full name, schema major, global payload size, and binding payload size; verify task 1.5 passes for literal and parameterized Topics.
- [x] 3.6 Replace the hard-coded operation Progress Topic check with a closed protocol-owned validator kind that compares payload and Envelope operation IDs, leaving other business payload bytes opaque; verify malformed Progress is rejected and an arbitrary matching business payload is never parsed.
- [x] 3.7 Add stable validation error codes and diagnostics for every structural, limit, reserved-header, catalog, and protocol-owned payload failure; verify table-driven tests assert both the error category and violated field or binding.

## 4. Put Validation Before Every Hub Side Effect

- [x] 4.1 Route every inbound CommHub frame through `ProtocolValidator` before authentication, registration, subscription, heartbeat, disconnect, return-route creation, routing, queueing, or QoS handling; verify the control- and application-path tests from task 1.6 pass.
- [x] 4.2 Add the internal Hub protocol-error status and map safe failures to the standard `REPLY/ERROR_DETAIL/industrial_bus.system.v1.Error` binding, dropping frames that lack sufficient reply metadata; verify replyable failures receive `PROTOCOL_ERROR` and unrepliable failures retain no state.
- [x] 4.3 Inject production and test `TopicCatalogView` instances explicitly so tests use scoped catalog bindings instead of adding arbitrary `test.*` Topics to the production manifest; verify the full existing runtime test suite passes under strict unknown-Topic rejection.
- [x] 4.4 Apply the meaningful structural, semantic, and catalog checks to outbound SDK encoding and surface local validation failures before transport; verify client tests reject SDK-created invalid/unknown bindings without sending bytes.
- [x] 4.5 Add end-to-end tests for valid literal and parameterized traffic plus each invalid binding class under subscription, request/reply, coalescing, overflow, and slow-consumer paths; verify valid QoS behavior is unchanged and rejected messages never enter those paths.

## 5. Add Compatibility Baselines and Offline CI Gates

- [x] 5.1 Check in the released v1 descriptor-set baseline, catalog baseline, and representative historical binary Envelope/payload fixtures under `protocol/compat/v1/`; verify each artifact has a documented reproducible update command and fixture expectations.
- [x] 5.2 Implement the descriptor compatibility checker for field-number reuse, incompatible type/cardinality changes, removal without reservation, and published enum removal/renumbering; verify the positive and negative cases from task 1.7 pass.
- [x] 5.3 Implement historical fixture decoding and unknown-field round-trip tests with current generated Protobuf code; verify every checked-in fixture preserves expected historical values and unknown fields.
- [x] 5.4 Implement catalog baseline comparison that permits compatible schema evolution and rejects payload or meaning changes under the same Topic/schema major; verify catalog compatibility cases from task 1.7 pass with old/new binding diagnostics.
- [x] 5.5 Register descriptor, fixture, catalog, ambiguity, determinism, and generated-output freshness checks with CMake/CTest and the existing GitHub Actions workflow; verify the standard offline CI command runs all checks without downloading new dependencies.

## 6. Documentation and Final Verification

- [x] 6.1 Document the catalog authoring/regeneration workflow, parameter rules, compatibility-baseline policy, `ProtocolLimits` defaults/overrides, protocol errors, and custom-Topic migration; verify every documented command works from a clean checkout.
- [x] 6.2 Run formatting/static checks plus the complete configure, build, and CTest suite; verify zero failures and record any environment-dependent skips with their prerequisite.
- [x] 6.3 Run strict OpenSpec validation for `implement-topic-catalog-protocol-validation`; verify the proposal, design, three capability specs, and completed task checklist remain coherent before archival.
