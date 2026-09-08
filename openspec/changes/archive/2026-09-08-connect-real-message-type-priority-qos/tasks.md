## 1. Establish Failing Contract Tests

- [x] 1.1 Add API/core tests for the complete MessageClass-to-MessageType/priority matrix, ErrorLog separation, and two-field EventTopic source compatibility; run `industrial_bus_core_tests` and confirm the new assertions fail before implementation.
- [x] 1.2 Add codec tests covering exact round trips for all supported MessageTypes, canonical priority and reserved semantic-class metadata, invalid type/priority rejection, and legacy header-less fallback; run `industrial_bus_runtime_e2e_test` and confirm the new cases fail before implementation.
- [x] 1.3 Add client-runtime tests for default Event, explicit Progress and Log classes, Command/Request/Reply priorities, non-terminal and terminal Operation priorities, and ProgressEvent serialization; run `industrial_bus_client_tests` and confirm the new cases fail before implementation.
- [x] 1.4 Expand CommHub QoS tests for cross-priority FIFO scheduling, same-operation Progress coalescing, semantic eviction order, P2/P3 drop, P1 rejection, P0 critical overflow, and all required statistics; run `industrial_bus_commhub_qos_test` and confirm the missing production behavior is exposed.
- [x] 1.5 Add Hub/server tests with multiple broadcast subscribers and failing transports to prove that critical overflow, normal drops, and send failure for one target do not suppress a healthy target; run the focused CommHub/runtime test targets and confirm the isolation cases fail before implementation.

## 2. Connect Semantic Classification to the Wire

- [x] 2.1 Extend the project-owned QoS model with ErrorLog and canonical semantic mapping helpers, add source-compatible topic/publish metadata, and verify the API/core mapping tests pass.
- [x] 2.2 Replace the lossy runtime message-kind model with exact supported MessageType values plus QosPriority and MessageClass on outbound/inbound messages; update client mocks and verify client code compiles without an Operation pseudo-type.
- [x] 2.3 Encode canonical MessageType, priority, and the reserved `industrial_bus.message_class` header; decode them without collapsing types, validate present metadata, and apply the documented legacy fallback when it is absent; verify all codec tests pass.
- [x] 2.4 Assign canonical P1 semantics to client and Hub heartbeat/system serializers and P0 semantics to Hub replies/errors, then verify heartbeat and registration/runtime tests pass.

## 3. Migrate SDK and Example Publishers

- [x] 3.1 Make ordinary publish default to `EVENT/P2`, allow only publication-safe explicit semantic classes, and assign SDK-controlled Command, Request, Reply, Ack, and error semantics internally; verify the client publication matrix passes.
- [x] 3.2 Make `reportOperation()` emit non-terminal OperationEvent messages as `EVENT/P2` and terminal messages as `EVENT/P0` while preserving the canonical payload type, role, protocol version, schema version, and `system.operation.event` topic; verify operation wire-contract tests pass.
- [x] 3.3 Add the dedicated `system.operation.progress` ProgressEvent path and migrate OperationGuard and ProgressThrottle to `PROGRESS/P2`; verify field serialization, matching operation IDs, and absence of a lifecycle transition in client/runtime tests.
- [x] 3.4 Classify simulated application Progress, completion/failure, and Log publications—including dynamic log severity—then verify simulation topic/adapter tests observe the expected MessageType and priority.

## 4. Make the Production QoS Policy Authoritative

- [x] 4.1 Extend the reusable bounded priority queue only as needed for production dequeue/outcome statistics and deterministic semantic eviction, then verify its unit tests cover accepted, dequeued, coalesced, dropped, rejected, critical-overflow, and high-watermark values.
- [x] 4.2 Refactor CommHub QoSScheduler to maintain one bounded semantic queue per destination, use validated MessageClass metadata and operation coalescing keys, and map queue outcomes to Hub dispositions; verify the expanded QoS scheduler tests pass.
- [x] 4.3 Replace the four independent capacity defaults with a documented per-connection bound that preserves approximately the current aggregate capacity, and verify zero/boundary capacity behavior in QoS tests.
- [x] 4.4 Validate MessageType, priority range, payload role, and semantic-class consistency before routing while accepting legacy header-less messages through the safe fallback; verify invalid messages never enter a destination queue or create a surviving return route.

## 5. Isolate Slow Consumers

- [x] 5.1 Change Hub broadcast dispatch to evaluate every destination independently, remove only a critical-overflow target, and retain directed P1 return-route cleanup; verify the multi-subscriber Hub tests pass.
- [x] 5.2 Make server draining continue across targets and disconnect only the target whose transport send fails, without retrying the popped frame; verify a healthy subscriber still receives the same broadcast in runtime tests.
- [x] 5.3 Verify session removal clears only the affected service registration, return routes, peer mapping, and QoS queue while other sessions remain routable.

## 6. Documentation and Final Verification

- [x] 6.1 Update module and simulated-communication documentation with the semantic mapping, `system.operation.progress` migration, overflow behavior, legacy fallback, and explicit exclusion of retry/status-query compensation; verify documented topics and payload types match the tests.
- [x] 6.2 Build all affected targets and run the complete CTest suite, recording any environment-dependent skips and requiring zero test failures.
- [x] 6.3 Run `openspec validate connect-real-message-type-priority-qos --strict` and verify the completed implementation still conforms to both delta specifications.
