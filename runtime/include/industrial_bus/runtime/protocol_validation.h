#pragma once

#include "industrial_bus/qos.h"
#include "industrial_bus/result.h"
#include "industrial_bus/topic_catalog.h"
#include "industrial_bus/transport/transport.h"

#include "envelope.pb.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace industrial_bus::runtime {

enum class ProtocolValidationCode : std::int32_t {
    FrameTooLarge = 1001,
    MalformedEnvelope = 1002,
    MissingField = 1003,
    InvalidIdentifier = 1004,
    FieldTooLong = 1005,
    UnsupportedVersion = 1006,
    InvalidTimestamp = 1007,
    InvalidDeadline = 1008,
    InvalidHopCount = 1009,
    TooManyHeaders = 1010,
    HeaderTooLarge = 1011,
    ReservedHeader = 1012,
    InvalidSemantics = 1013,
    UnknownTopic = 1014,
    BindingMismatch = 1015,
    PayloadTooLarge = 1016,
    ProtocolPayloadInvalid = 1017,
};

std::string_view protocolValidationCodeName(ProtocolValidationCode code) noexcept;

struct ProtocolLimits {
    std::size_t max_frame_bytes{2U * 1024U * 1024U};
    std::size_t max_payload_bytes{1U * 1024U * 1024U};
    std::size_t max_identifier_bytes{128};
    std::size_t max_service_bytes{128};
    std::size_t max_instance_bytes{128};
    std::size_t max_idempotency_key_bytes{128};
    std::size_t max_topic_bytes{256};
    std::size_t max_payload_type_bytes{256};
    std::size_t max_header_count{32};
    std::size_t max_header_key_bytes{64};
    std::size_t max_header_value_bytes{1024};
    std::size_t max_header_aggregate_bytes{16U * 1024U};
    std::uint32_t max_allowed_hops{8};
    std::chrono::nanoseconds max_future_timestamp_skew{std::chrono::minutes(5)};
    std::chrono::nanoseconds max_deadline_horizon{std::chrono::hours(24)};
    std::uint32_t min_protocol_version{1};
    std::uint32_t max_protocol_version{1};
};

struct ValidatedEnvelope {
    protocol::v1::MessageEnvelope wire;
    MessageType message_type{MessageType::Event};
    MessageClass message_class{MessageClass::Event};
    QosPriority priority{QosPriority::Normal};
    const TopicBinding* binding{nullptr};
};

class ProtocolValidator {
public:
    explicit ProtocolValidator(
        TopicCatalogView catalog = productionTopicCatalog(),
        ProtocolLimits limits = {});

    Result<ValidatedEnvelope> validate(
        const transport::ByteBuffer& frame,
        std::int64_t now_unix_ns) const;
    Result<ValidatedEnvelope> validate(
        const transport::ByteBuffer& frame) const;

    TopicCatalogView catalog() const noexcept { return catalog_; }
    const ProtocolLimits& limits() const noexcept { return limits_; }

private:
    TopicCatalogView catalog_;
    ProtocolLimits limits_;
};

const ProtocolValidator& productionProtocolValidator();

}  // namespace industrial_bus::runtime
