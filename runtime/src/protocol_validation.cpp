#include "industrial_bus/runtime/protocol_validation.h"

#include "industrial_bus/ids.h"

#include "operation.pb.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>

namespace industrial_bus::runtime {
namespace {

namespace envelope = industrial_bus::protocol::v1;
namespace operation_proto = industrial_bus::operation::v1;

constexpr char kMessageClassHeader[] = "industrial_bus.message_class";
constexpr char kReservedPrefix[] = "industrial_bus.";
constexpr char kAuthorizationHeader[] = "authorization";
constexpr char kRegistrationTopic[] = "system.service.register";

std::int64_t unixNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Error failure(ProtocolValidationCode code, std::string field,
              std::string message) {
    auto error = Error::protocol(std::move(message), static_cast<std::int32_t>(code));
    error.context["protocol_code"] =
        std::string(protocolValidationCodeName(code));
    error.context["field"] = std::move(field);
    return error;
}

Result<MessageType> fromWire(envelope::MessageType type) {
    switch (type) {
        case envelope::COMMAND: return MessageType::Command;
        case envelope::EVENT: return MessageType::Event;
        case envelope::REQUEST: return MessageType::Request;
        case envelope::REPLY: return MessageType::Reply;
        case envelope::PROGRESS: return MessageType::Progress;
        case envelope::ERROR: return MessageType::Error;
        case envelope::LOG: return MessageType::Log;
        case envelope::HEARTBEAT: return MessageType::Heartbeat;
        case envelope::SYSTEM: return MessageType::System;
        case envelope::MESSAGE_TYPE_UNSPECIFIED:
        default:
            return failure(ProtocolValidationCode::InvalidSemantics,
                           "message_type",
                           "unsupported or unspecified MessageType");
    }
}

CatalogPayloadRole catalogRole(envelope::PayloadRole role) noexcept {
    return static_cast<CatalogPayloadRole>(static_cast<std::uint8_t>(role));
}

bool roleMatches(MessageType type, envelope::PayloadRole role) noexcept {
    switch (type) {
        case MessageType::Command: return role == envelope::COMMAND_INPUT;
        case MessageType::Request: return role == envelope::REQUEST_INPUT;
        case MessageType::Reply:
            return role == envelope::COMMAND_ACK ||
                   role == envelope::RESPONSE_OUTPUT ||
                   role == envelope::ERROR_DETAIL ||
                   role == envelope::SYSTEM_PAYLOAD;
        case MessageType::Event:
        case MessageType::Progress:
        case MessageType::Log: return role == envelope::EVENT_FACT;
        case MessageType::Error: return role == envelope::ERROR_DETAIL;
        case MessageType::Heartbeat:
        case MessageType::System: return role == envelope::SYSTEM_PAYLOAD;
    }
    return false;
}

bool typeMatchesClass(MessageType type, MessageClass message_class) noexcept {
    return messageTypeFor(message_class) == type ||
           (type == MessageType::Reply && message_class == MessageClass::Error);
}

MessageClass legacyClass(MessageType type, envelope::PayloadRole role,
                         std::uint32_t priority) noexcept {
    switch (type) {
        case MessageType::Command: return MessageClass::Command;
        case MessageType::Event: return MessageClass::Event;
        case MessageType::Request: return MessageClass::Request;
        case MessageType::Reply:
            if (role == envelope::COMMAND_ACK) return MessageClass::Accepted;
            if (role == envelope::ERROR_DETAIL) return MessageClass::Error;
            return MessageClass::Reply;
        case MessageType::Progress: return MessageClass::Progress;
        case MessageType::Error: return MessageClass::Error;
        case MessageType::Log:
            return priority == static_cast<std::uint32_t>(QosPriority::BestEffort)
                       ? MessageClass::InfoLog
                       : MessageClass::WarnLog;
        case MessageType::Heartbeat: return MessageClass::Heartbeat;
        case MessageType::System: return MessageClass::ServiceState;
    }
    return MessageClass::Event;
}

struct Semantics {
    MessageType type;
    MessageClass message_class;
    QosPriority priority;
};

Result<Semantics> validateSemantics(const envelope::MessageEnvelope& wire) {
    auto type = fromWire(wire.message_type());
    if (!type) return type.error();
    if (!roleMatches(type.value(), wire.payload_role())) {
        return failure(ProtocolValidationCode::InvalidSemantics, "payload_role",
                       "MessageType and PayloadRole are inconsistent");
    }
    if (wire.priority() > static_cast<std::uint32_t>(QosPriority::BestEffort)) {
        return failure(ProtocolValidationCode::InvalidSemantics, "priority",
                       "Envelope priority is outside P0 through P3");
    }

    const auto header = wire.headers().find(kMessageClassHeader);
    if (header != wire.headers().end()) {
        const auto parsed_class = messageClassFromName(header->second);
        if (!parsed_class || !typeMatchesClass(type.value(), *parsed_class) ||
            wire.priority() !=
                static_cast<std::uint32_t>(defaultPriority(*parsed_class))) {
            return failure(
                ProtocolValidationCode::InvalidSemantics,
                kMessageClassHeader,
                "MessageType, priority and semantic class are inconsistent");
        }
        return Semantics{type.value(), *parsed_class,
                         defaultPriority(*parsed_class)};
    }

    const auto message_class =
        legacyClass(type.value(), wire.payload_role(), wire.priority());
    const auto canonical_priority = defaultPriority(message_class);
    const auto supplied_priority = static_cast<QosPriority>(wire.priority());
    const bool accepted_legacy_default =
        supplied_priority == QosPriority::Critical &&
        (type.value() == MessageType::Command ||
         type.value() == MessageType::Request ||
         type.value() == MessageType::Event ||
         type.value() == MessageType::Heartbeat ||
         type.value() == MessageType::System);
    if (supplied_priority != canonical_priority && !accepted_legacy_default) {
        return failure(ProtocolValidationCode::InvalidSemantics, "priority",
                       "legacy Envelope priority is inconsistent");
    }
    return Semantics{type.value(), message_class, canonical_priority};
}

bool tooLong(const std::string& value, std::size_t limit) noexcept {
    return value.size() > limit;
}

Result<void> validateOptionalId(const std::string& value,
                                std::string field,
                                std::size_t max_length,
                                bool required = false) {
    if (value.empty()) {
        return required
                   ? Result<void>(failure(ProtocolValidationCode::MissingField,
                                          std::move(field),
                                          "required protocol identifier is empty"))
                   : Result<void>::success();
    }
    if (!isValidProtocolId(value, max_length)) {
        return failure(ProtocolValidationCode::InvalidIdentifier,
                       std::move(field),
                       "protocol identifier has invalid characters or length");
    }
    return Result<void>::success();
}

Result<void> validateHeaders(const envelope::MessageEnvelope& wire,
                             const ProtocolLimits& limits) {
    if (static_cast<std::size_t>(wire.headers_size()) > limits.max_header_count) {
        return failure(ProtocolValidationCode::TooManyHeaders, "headers",
                       "Envelope has too many headers");
    }
    std::size_t aggregate = 0;
    for (const auto& item : wire.headers()) {
        if (item.first.empty() || item.first.size() > limits.max_header_key_bytes ||
            item.second.size() > limits.max_header_value_bytes) {
            return failure(ProtocolValidationCode::HeaderTooLarge,
                           "headers." + item.first,
                           "Envelope header key or value exceeds its limit");
        }
        if (aggregate > limits.max_header_aggregate_bytes -
                            std::min(limits.max_header_aggregate_bytes,
                                     item.first.size() + item.second.size())) {
            return failure(ProtocolValidationCode::HeaderTooLarge, "headers",
                           "Envelope aggregate header bytes exceed the limit");
        }
        aggregate += item.first.size() + item.second.size();
        if (item.first.compare(0, sizeof(kReservedPrefix) - 1, kReservedPrefix) == 0 &&
            item.first != kMessageClassHeader) {
            return failure(ProtocolValidationCode::ReservedHeader, item.first,
                           "unknown reserved industrial_bus header");
        }
        if (item.first == kAuthorizationHeader &&
            (wire.topic() != kRegistrationTopic ||
             wire.message_type() != envelope::SYSTEM ||
             wire.payload_role() != envelope::SYSTEM_PAYLOAD)) {
            return failure(ProtocolValidationCode::ReservedHeader, item.first,
                           "authorization is only valid on service registration");
        }
    }
    if (aggregate > limits.max_header_aggregate_bytes) {
        return failure(ProtocolValidationCode::HeaderTooLarge, "headers",
                       "Envelope aggregate header bytes exceed the limit");
    }
    return Result<void>::success();
}

Result<void> validateTypeFields(const envelope::MessageEnvelope& wire,
                                MessageType type,
                                std::int64_t now,
                                const ProtocolLimits& limits) {
    const bool directed = !wire.destination_service().empty();
    const bool has_deadline = wire.deadline_unix_ns() != 0;
    const bool command_or_request = type == MessageType::Command ||
                                    type == MessageType::Request;
    if (command_or_request && !directed) {
        return failure(ProtocolValidationCode::MissingField,
                       "destination_service",
                       "Command and Request require a destination service");
    }
    if (type == MessageType::Reply && !directed) {
        return failure(ProtocolValidationCode::MissingField,
                       "destination_service",
                       "Reply requires a destination service");
    }
    if (!command_or_request && type != MessageType::Reply && directed) {
        return failure(ProtocolValidationCode::InvalidSemantics,
                       "destination_service",
                       "this MessageType must not carry a destination");
    }
    if (type == MessageType::Command && wire.idempotency_key().empty()) {
        return failure(ProtocolValidationCode::MissingField, "idempotency_key",
                       "Command requires an idempotency key");
    }
    if (type != MessageType::Command && !wire.idempotency_key().empty()) {
        return failure(ProtocolValidationCode::InvalidSemantics,
                       "idempotency_key",
                       "only Command may carry an idempotency key");
    }
    if (command_or_request && !has_deadline) {
        return failure(ProtocolValidationCode::MissingField, "deadline_unix_ns",
                       "Command and Request require a deadline");
    }
    if (!command_or_request && has_deadline) {
        return failure(ProtocolValidationCode::InvalidDeadline,
                       "deadline_unix_ns",
                       "only Command and Request may carry a deadline");
    }
    if (has_deadline &&
        (wire.deadline_unix_ns() <= now ||
         wire.deadline_unix_ns() - now > limits.max_deadline_horizon.count())) {
        return failure(ProtocolValidationCode::InvalidDeadline,
                       "deadline_unix_ns",
                       "deadline is expired or exceeds the configured horizon");
    }
    if (type == MessageType::Reply && wire.causation_id().empty()) {
        return failure(ProtocolValidationCode::MissingField, "causation_id",
                       "Reply requires a causation identifier");
    }
    if (type != MessageType::Reply && !wire.causation_id().empty()) {
        return failure(ProtocolValidationCode::InvalidSemantics, "causation_id",
                       "only Reply may carry a causation identifier");
    }
    if (type == MessageType::Progress && wire.operation_id().empty()) {
        return failure(ProtocolValidationCode::MissingField, "operation_id",
                       "Progress requires an operation identifier");
    }
    return Result<void>::success();
}

}  // namespace

std::string_view protocolValidationCodeName(ProtocolValidationCode code) noexcept {
    switch (code) {
        case ProtocolValidationCode::FrameTooLarge: return "frame_too_large";
        case ProtocolValidationCode::MalformedEnvelope: return "malformed_envelope";
        case ProtocolValidationCode::MissingField: return "missing_field";
        case ProtocolValidationCode::InvalidIdentifier: return "invalid_identifier";
        case ProtocolValidationCode::FieldTooLong: return "field_too_long";
        case ProtocolValidationCode::UnsupportedVersion: return "unsupported_version";
        case ProtocolValidationCode::InvalidTimestamp: return "invalid_timestamp";
        case ProtocolValidationCode::InvalidDeadline: return "invalid_deadline";
        case ProtocolValidationCode::InvalidHopCount: return "invalid_hop_count";
        case ProtocolValidationCode::TooManyHeaders: return "too_many_headers";
        case ProtocolValidationCode::HeaderTooLarge: return "header_too_large";
        case ProtocolValidationCode::ReservedHeader: return "reserved_header";
        case ProtocolValidationCode::InvalidSemantics: return "invalid_semantics";
        case ProtocolValidationCode::UnknownTopic: return "unknown_topic";
        case ProtocolValidationCode::BindingMismatch: return "binding_mismatch";
        case ProtocolValidationCode::PayloadTooLarge: return "payload_too_large";
        case ProtocolValidationCode::ProtocolPayloadInvalid:
            return "protocol_payload_invalid";
    }
    return "unknown";
}

ProtocolValidator::ProtocolValidator(TopicCatalogView catalog,
                                     ProtocolLimits limits)
    : catalog_(catalog), limits_(limits) {}

Result<ValidatedEnvelope> ProtocolValidator::validate(
    const transport::ByteBuffer& frame) const {
    return validate(frame, unixNowNs());
}

Result<ValidatedEnvelope> ProtocolValidator::validate(
    const transport::ByteBuffer& frame, std::int64_t now) const {
    if (frame.size() > limits_.max_frame_bytes) {
        return failure(ProtocolValidationCode::FrameTooLarge, "frame",
                       "MessageEnvelope frame exceeds ProtocolLimits");
    }
    envelope::MessageEnvelope wire;
    if (frame.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !wire.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
        return failure(ProtocolValidationCode::MalformedEnvelope, "frame",
                       "invalid MessageEnvelope frame");
    }

    for (const auto& field : {
             std::pair<const std::string*, const char*>{&wire.message_id(), "message_id"},
             {&wire.correlation_id(), "correlation_id"}}) {
        auto valid = validateOptionalId(*field.first, field.second,
                                        limits_.max_identifier_bytes, true);
        if (!valid) return valid.error();
    }
    for (const auto& field : {
             std::pair<const std::string*, const char*>{&wire.operation_id(), "operation_id"},
             {&wire.causation_id(), "causation_id"},
             {&wire.source_node(), "source_node"},
             {&wire.destination_node(), "destination_node"}}) {
        auto valid = validateOptionalId(*field.first, field.second,
                                        limits_.max_identifier_bytes);
        if (!valid) return valid.error();
    }
    for (const auto& field : {
             std::tuple<const std::string*, const char*, std::size_t, bool>{
                 &wire.source_service(), "source_service",
                 limits_.max_service_bytes, true},
             {&wire.source_instance(), "source_instance",
              limits_.max_instance_bytes, true},
             {&wire.destination_service(), "destination_service",
              limits_.max_service_bytes, false},
             {&wire.destination_instance(), "destination_instance",
              limits_.max_instance_bytes, false}}) {
        auto valid = validateOptionalId(*std::get<0>(field), std::get<1>(field),
                                        std::get<2>(field), std::get<3>(field));
        if (!valid) return valid.error();
    }
    if (!wire.idempotency_key().empty() &&
        !isValidProtocolId(wire.idempotency_key(),
                           limits_.max_idempotency_key_bytes)) {
        return failure(ProtocolValidationCode::InvalidIdentifier,
                       "idempotency_key",
                       "idempotency key has invalid characters or length");
    }
    if (wire.topic().empty() || wire.payload_type().empty()) {
        return failure(ProtocolValidationCode::MissingField,
                       wire.topic().empty() ? "topic" : "payload_type",
                       "Envelope requires topic and payload_type");
    }
    if (tooLong(wire.topic(), limits_.max_topic_bytes) ||
        tooLong(wire.payload_type(), limits_.max_payload_type_bytes) ||
        tooLong(wire.source_service(), limits_.max_service_bytes) ||
        tooLong(wire.destination_service(), limits_.max_service_bytes) ||
        tooLong(wire.source_instance(), limits_.max_instance_bytes) ||
        tooLong(wire.destination_instance(), limits_.max_instance_bytes) ||
        tooLong(wire.idempotency_key(), limits_.max_idempotency_key_bytes)) {
        return failure(ProtocolValidationCode::FieldTooLong, "metadata",
                       "Envelope metadata exceeds ProtocolLimits");
    }
    if (wire.protocol_version() < limits_.min_protocol_version ||
        wire.protocol_version() > limits_.max_protocol_version) {
        return failure(ProtocolValidationCode::UnsupportedVersion,
                       "protocol_version", "unsupported protocol version");
    }
    if (wire.schema_version() == 0) {
        return failure(ProtocolValidationCode::UnsupportedVersion,
                       "schema_version", "schema major must be non-zero");
    }
    if (wire.timestamp_unix_ns() <= 0 ||
        wire.timestamp_unix_ns() > now + limits_.max_future_timestamp_skew.count()) {
        return failure(ProtocolValidationCode::InvalidTimestamp,
                       "timestamp_unix_ns",
                       "timestamp is absent or too far in the future");
    }
    if (wire.max_hops() > limits_.max_allowed_hops ||
        wire.hop_count() > wire.max_hops()) {
        return failure(ProtocolValidationCode::InvalidHopCount, "hop_count",
                       "Envelope hop bounds are invalid");
    }
    if (wire.payload().size() > limits_.max_payload_bytes) {
        return failure(ProtocolValidationCode::PayloadTooLarge, "payload",
                       "payload exceeds the global ProtocolLimits bound");
    }
    auto headers = validateHeaders(wire, limits_);
    if (!headers) return headers.error();
    auto semantics = validateSemantics(wire);
    if (!semantics) return semantics.error();
    auto fields = validateTypeFields(wire, semantics.value().type, now, limits_);
    if (!fields) return fields.error();

    if (!catalog_.containsTopic(wire.topic())) {
        return failure(ProtocolValidationCode::UnknownTopic, "topic",
                       "Topic is not registered in the Topic Catalog");
    }
    const auto* binding = catalog_.find(wire.topic(), semantics.value().type,
                                        catalogRole(wire.payload_role()));
    if (binding == nullptr || binding->payload_type != wire.payload_type() ||
        binding->schema_major != wire.schema_version()) {
        return failure(ProtocolValidationCode::BindingMismatch, "binding",
                       "Topic, MessageType, payload role, payload type and schema major do not match a catalog binding");
    }
    if (wire.payload().size() > binding->max_payload_bytes) {
        return failure(ProtocolValidationCode::PayloadTooLarge, "payload",
                       "payload exceeds its Topic Catalog binding limit");
    }
    if (binding->validator == ProtocolPayloadValidatorKind::OperationProgressId) {
        operation_proto::ProgressEvent progress;
        if (!progress.ParseFromString(wire.payload()) ||
            progress.operation_id() != wire.operation_id()) {
            return failure(ProtocolValidationCode::ProtocolPayloadInvalid,
                           "payload.operation_id",
                           "ProgressEvent operation_id must match its Envelope");
        }
    }

    return ValidatedEnvelope{std::move(wire), semantics.value().type,
                             semantics.value().message_class,
                             semantics.value().priority, binding};
}

const ProtocolValidator& productionProtocolValidator() {
    static const ProtocolValidator validator;
    return validator;
}

}  // namespace industrial_bus::runtime
