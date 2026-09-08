# Topic Catalog and protocol validation

IndustrialBus routes only contracts declared in
`protocol/catalog/topics.textproto`. The catalog is the authoritative source
for the checked-in typed API in
`api/include/industrial_bus/topics.generated.h` and the immutable runtime table
in `runtime/src/topic_catalog.generated.inc`.

## Authoring and generation

Each binding declares a stable symbol, literal Topic or whole-segment pattern,
MessageType, payload role, Protobuf full name, schema major, payload limit, and
optional protocol-owned validator. Command and Request inputs must have both a
successful Reply binding and a standard Error Reply binding.

Parameterized Topics use a complete segment such as `{channel_uuid}`. Values
must be RFC 4122 UUIDs; empty values, dots, braces, and wildcards are rejected.
Literal bindings take precedence, while overlapping parameterized patterns are
rejected during generation.

After editing the catalog or a referenced Protobuf type, regenerate and check
the outputs:

```sh
cmake --preset debug
cmake --build build/debug --target industrial_bus_generate_topic_catalog
ctest --test-dir build/debug -R TopicCatalogGeneratedOutputsCurrent --output-on-failure
```

Applications should use declarations from `industrial_bus/topics.generated.h`.
The existing `EventTopic`, `CommandTopic`, and `RequestTopic` aggregate types
remain source compatible, but constructing one directly does not register it.
A custom Topic must first be added to the catalog and regenerated or CommHub
and the transport runtime reject it locally as `PROTOCOL_ERROR`.

Tests that intentionally use private Topics inject a scoped `TopicCatalogView`
through `CommHubServerOptions::topic_catalog` and
`TransportRuntimeOptions::topic_catalog`; test-only bindings do not belong in
the production manifest.

## Validation boundary and order

`ProtocolValidator` runs on every CommHub frame before authentication, control
handling, service/session mutation, return-route creation, routing, queueing,
coalescing, overflow handling, or delivery. The transport runtime also applies
the meaningful checks before sending SDK-created application messages.

Validation proceeds in this order:

1. frame size before Protobuf parsing;
2. required fields, identifier syntax, lengths/counts, version, time, deadline,
   hop, and type-specific metadata;
3. MessageType, payload role, semantic class, and canonical priority;
4. exact catalog binding and global/per-binding payload size;
5. a closed protocol-owned payload validator, when declared.

Ordinary business payload bytes stay opaque. The initial closed validator set
contains only the operation Progress check that requires payload and Envelope
operation IDs to match. The legacy header-less semantic-class fallback remains
available for otherwise valid known bindings and cannot bypass catalog, schema,
or limit checks.

Reserved headers use the `industrial_bus.` prefix. Currently
`industrial_bus.message_class` is the registered reserved header; unknown keys
in that namespace are rejected. The unprefixed registration `authorization`
header remains restricted to the service-registration control path.

When enough source and Topic metadata is trustworthy and the catalog declares
an Error Reply binding, CommHub returns `REPLY + ERROR_DETAIL +
industrial_bus.system.v1.Error` with category `PROTOCOL_ERROR`. Otherwise the
frame is dropped. In either case no state from the rejected frame is retained.
Diagnostics expose a stable numeric code plus `protocol_code` and `field`
context values.

## ProtocolLimits defaults

| Limit | Default |
| --- | ---: |
| Serialized frame | 2 MiB |
| Payload, before per-binding restriction | 1 MiB |
| Protocol IDs, service, instance, idempotency key | 128 bytes each |
| Topic and payload full name | 256 bytes each |
| Header count | 32 |
| Header key / value | 64 / 1024 bytes |
| Aggregate headers | 16 KiB |
| Maximum declared hops | 8 |
| Future timestamp skew | 5 minutes |
| Command/Request deadline horizon | 24 hours |
| Supported protocol version | 1 |

Limits are inclusive: a value exactly at its configured boundary is accepted.
Tests and embedded deployments may inject a complete `ProtocolLimits` value
through the same server/runtime option structures; production defaults stay
centralized in `protocol_validation.h`.

## Compatibility policy

`protocol/compat/v1/` stores the released descriptor set, catalog baseline,
and historical binary Envelope/payload fixtures. CTest rejects field-number
reuse, incompatible type/cardinality changes, removal without reserving both
name and number, published enum removal/renumbering, removed catalog bindings,
in-place payload/validator changes, narrowed payload limits, stale generated
outputs, and ambiguous catalog patterns. Fixture tests also preserve unknown
fields across decode/re-encode.

Run all offline compatibility gates with:

```sh
cmake --build --preset debug --parallel 4
ctest --preset debug --no-tests=error
```

After an intentionally published compatible v1 update, refresh baselines only
after reviewing the schema/catalog diff:

```sh
cmake --build build/debug --target industrial_bus_update_compat_baseline
```

The checks use only repository tools and the locally installed CMake/Protobuf/
GoogleTest toolchain; they do not download a schema service or generator.
