## 1. Regression Coverage

- [x] 1.1 Add a BusClient regression test that reports an operation update and verifies the outbound topic remains `system.operation.event` while `payload_type` equals the OperationEvent Protobuf descriptor full name; build and run `industrial_bus_client_tests` to verify the test.
- [x] 1.2 Add an envelope codec regression test that verifies the canonical payload type survives serialization and that the payload decodes as `industrial_bus.operation.v1.OperationEvent`; build and run `industrial_bus_runtime_e2e_test` to verify the wire contract.

## 2. Metadata Correction

- [x] 2.1 Update `BusClient::reportOperation()` to source its payload type from the OperationEvent Protobuf descriptor without changing its topic, MessageType, PayloadRole, or schema; verify both regression test targets pass.

## 3. Verification

- [x] 3.1 Run `ctest --test-dir build --output-on-failure` and verify the full available suite passes, then search non-generated source to confirm the invalid `industrial_bus.system.v1.OperationEvent` name is absent.
