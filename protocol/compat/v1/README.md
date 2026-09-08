# Protocol v1 compatibility baseline

This directory contains the released v1 descriptor set, Topic Catalog, and
representative binary wire fixtures used by the offline CTest compatibility
gates.

After intentionally publishing a compatible v1 protocol/catalog update, review
the diff and refresh the baseline with:

```sh
cmake --preset debug
cmake --build build/debug --target industrial_bus_update_compat_baseline
```

Regenerate only the historical binary fixtures with:

```sh
cmake --build build/debug --target industrial_bus_update_protocol_fixtures
```

`envelope_v1.bin` contains an Event Envelope with a serialized
`OperationEvent`; both messages include unknown field 250 so the fixture tests
also verify unknown-field round trips. Baseline changes are release-contract
changes and must be reviewed together with schema and catalog compatibility.
