#include "industrial_bus/runtime/protocol_validation.h"
#include "industrial_bus/runtime/envelope_codec.h"

#include "industrial_bus/ids.h"

#include "operation.pb.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace {

using namespace industrial_bus;
namespace wire = industrial_bus::protocol::v1;

constexpr std::int64_t kNow = 2'000'000'000'000'000'000LL;

transport::ByteBuffer serialize(const wire::MessageEnvelope& envelope) {
    std::string bytes;
    EXPECT_TRUE(envelope.SerializeToString(&bytes));
    return {bytes.begin(), bytes.end()};
}

wire::MessageEnvelope validEvent(std::string topic = "test.valid") {
    wire::MessageEnvelope envelope;
    envelope.set_message_id("message-1");
    envelope.set_correlation_id("correlation-1");
    envelope.set_source_node("node-1");
    envelope.set_source_service("publisher");
    envelope.set_source_instance("instance-1");
    envelope.set_topic(std::move(topic));
    envelope.set_message_type(wire::EVENT);
    envelope.set_timestamp_unix_ns(kNow);
    envelope.set_protocol_version(1);
    envelope.set_schema_version(1);
    envelope.set_priority(2);
    envelope.set_max_hops(1);
    (*envelope.mutable_headers())["industrial_bus.message_class"] = "Event";
    envelope.set_payload_type("test.Event");
    envelope.set_payload("opaque-payload");
    envelope.set_payload_role(wire::EVENT_FACT);
    return envelope;
}

TopicBinding eventBinding(std::string_view pattern = "test.valid",
                          std::size_t max_payload = 4096) {
    return {"kTestEvent", pattern, MessageType::Event,
            CatalogPayloadRole::EventFact, "test.Event", 1, max_payload,
            ProtocolPayloadValidatorKind::None};
}

Result<runtime::ValidatedEnvelope> validate(
    const wire::MessageEnvelope& envelope,
    const TopicBinding& binding,
    runtime::ProtocolLimits limits = {}) {
    const TopicCatalogView catalog(&binding, 1);
    return runtime::ProtocolValidator(catalog, limits).validate(serialize(envelope),
                                                                 kNow);
}

template <typename T>
void expectCode(const Result<T>& result, runtime::ProtocolValidationCode code,
                std::string_view field = {}) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, ErrorCategory::ProtocolError);
    EXPECT_EQ(result.error().code, static_cast<std::int32_t>(code));
    const auto named = result.error().context.find("protocol_code");
    ASSERT_NE(named, result.error().context.end());
    EXPECT_EQ(named->second, runtime::protocolValidationCodeName(code));
    if (!field.empty()) {
        const auto failed_field = result.error().context.find("field");
        ASSERT_NE(failed_field, result.error().context.end());
        EXPECT_EQ(failed_field->second, field);
    }
}

TEST(ProtocolLimitsTest, ExposesVersionedConservativeDefaults) {
    const runtime::ProtocolLimits limits;
    EXPECT_EQ(limits.max_frame_bytes, 2U * 1024U * 1024U);
    EXPECT_EQ(limits.max_payload_bytes, 1024U * 1024U);
    EXPECT_EQ(limits.max_identifier_bytes, 128U);
    EXPECT_EQ(limits.max_service_bytes, 128U);
    EXPECT_EQ(limits.max_instance_bytes, 128U);
    EXPECT_EQ(limits.max_idempotency_key_bytes, 128U);
    EXPECT_EQ(limits.max_topic_bytes, 256U);
    EXPECT_EQ(limits.max_payload_type_bytes, 256U);
    EXPECT_EQ(limits.max_header_count, 32U);
    EXPECT_EQ(limits.max_header_key_bytes, 64U);
    EXPECT_EQ(limits.max_header_value_bytes, 1024U);
    EXPECT_EQ(limits.max_header_aggregate_bytes, 16U * 1024U);
    EXPECT_EQ(limits.max_allowed_hops, 8U);
    EXPECT_EQ(limits.max_future_timestamp_skew, std::chrono::minutes(5));
    EXPECT_EQ(limits.max_deadline_horizon, std::chrono::hours(24));
    EXPECT_EQ(limits.min_protocol_version, 1U);
    EXPECT_EQ(limits.max_protocol_version, 1U);
}

TEST(ProtocolLimitsTest, AcceptsExactFrameAndPayloadBoundaries) {
    auto envelope = validEvent();
    envelope.set_payload("12345678");
    auto binding = eventBinding("test.valid", envelope.payload().size());
    auto frame = serialize(envelope);
    runtime::ProtocolLimits limits;
    limits.max_frame_bytes = frame.size();
    limits.max_payload_bytes = envelope.payload().size();
    EXPECT_TRUE(runtime::ProtocolValidator({&binding, 1}, limits)
                    .validate(frame, kNow));

    limits.max_frame_bytes = frame.size() - 1;
    expectCode(runtime::ProtocolValidator({&binding, 1}, limits)
                   .validate(frame, kNow),
               runtime::ProtocolValidationCode::FrameTooLarge, "frame");
    limits.max_frame_bytes = frame.size();
    limits.max_payload_bytes = envelope.payload().size() - 1;
    expectCode(runtime::ProtocolValidator({&binding, 1}, limits)
                   .validate(frame, kNow),
               runtime::ProtocolValidationCode::PayloadTooLarge, "payload");

    limits.max_payload_bytes = envelope.payload().size();
    binding.max_payload_bytes = envelope.payload().size() - 1;
    expectCode(runtime::ProtocolValidator({&binding, 1}, limits)
                   .validate(frame, kNow),
               runtime::ProtocolValidationCode::PayloadTooLarge, "payload");
}

TEST(ProtocolLimitsTest, EnforcesStringHeaderHopVersionAndTimeLimits) {
    auto envelope = validEvent();
    auto binding = eventBinding();
    runtime::ProtocolLimits limits;
    limits.max_identifier_bytes = 32;
    limits.max_topic_bytes = envelope.topic().size();
    limits.max_payload_type_bytes = envelope.payload_type().size();
    limits.max_header_count = 1;
    limits.max_header_key_bytes = std::string("industrial_bus.message_class").size();
    limits.max_header_value_bytes = std::string("Event").size();
    limits.max_header_aggregate_bytes =
        std::string("industrial_bus.message_class").size() + 5;
    EXPECT_TRUE(validate(envelope, binding, limits));

    auto invalid = envelope;
    invalid.set_message_id(std::string(limits.max_identifier_bytes + 1, 'm'));
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::InvalidIdentifier,
               "message_id");

    invalid = envelope;
    (*invalid.mutable_headers())["extra"] = "x";
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::TooManyHeaders, "headers");

    invalid = envelope;
    invalid.set_hop_count(2);
    invalid.set_max_hops(1);
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::InvalidHopCount, "hop_count");

    invalid = envelope;
    invalid.set_max_hops(limits.max_allowed_hops + 1);
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::InvalidHopCount, "hop_count");

    invalid = envelope;
    invalid.set_protocol_version(2);
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::UnsupportedVersion,
               "protocol_version");

    invalid = envelope;
    invalid.set_timestamp_unix_ns(
        kNow + limits.max_future_timestamp_skew.count() + 1);
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::InvalidTimestamp,
               "timestamp_unix_ns");
}

TEST(ProtocolLimitsTest, EnforcesEveryTextAndHeaderDimension) {
    auto envelope = validEvent();
    auto binding = eventBinding();
    runtime::ProtocolLimits limits;
    EXPECT_TRUE(validate(envelope, binding, limits));

    auto invalid = envelope;
    invalid.set_message_id("invalid id with spaces");
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::InvalidIdentifier,
               "message_id");

    limits.max_service_bytes = envelope.source_service().size();
    EXPECT_TRUE(validate(envelope, binding, limits));
    invalid = envelope;
    invalid.set_source_service(envelope.source_service() + "x");
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::InvalidIdentifier,
               "source_service");

    limits = {};
    limits.max_instance_bytes = envelope.source_instance().size();
    EXPECT_TRUE(validate(envelope, binding, limits));
    invalid = envelope;
    invalid.set_source_instance(envelope.source_instance() + "x");
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::InvalidIdentifier,
               "source_instance");

    limits = {};
    limits.max_topic_bytes = envelope.topic().size();
    EXPECT_TRUE(validate(envelope, binding, limits));
    invalid = envelope;
    invalid.set_topic(envelope.topic() + "x");
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::FieldTooLong, "metadata");

    limits = {};
    limits.max_payload_type_bytes = envelope.payload_type().size();
    EXPECT_TRUE(validate(envelope, binding, limits));
    invalid = envelope;
    invalid.set_payload_type(envelope.payload_type() + "x");
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::FieldTooLong, "metadata");

    limits = {};
    invalid = envelope;
    (*invalid.mutable_headers())[std::string(limits.max_header_key_bytes + 1, 'k')] =
        "x";
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::HeaderTooLarge);
    invalid = envelope;
    (*invalid.mutable_headers())["plain"] =
        std::string(limits.max_header_value_bytes + 1, 'v');
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::HeaderTooLarge);
    invalid = envelope;
    (*invalid.mutable_headers())["plain"] = "x";
    limits.max_header_aggregate_bytes =
        std::string("industrial_bus.message_class").size() +
        std::string("Event").size();
    expectCode(validate(invalid, binding, limits),
               runtime::ProtocolValidationCode::HeaderTooLarge, "headers");

    auto command_binding = TopicBinding{
        "kCommand", "test.command", MessageType::Command,
        CatalogPayloadRole::CommandInput, "test.Command", 1, 4096,
        ProtocolPayloadValidatorKind::None};
    auto command = validEvent("test.command");
    command.set_message_type(wire::COMMAND);
    command.set_payload_role(wire::COMMAND_INPUT);
    command.set_payload_type("test.Command");
    command.set_priority(1);
    (*command.mutable_headers())["industrial_bus.message_class"] = "Command";
    command.set_destination_service("worker");
    command.set_idempotency_key("intent");
    command.set_deadline_unix_ns(kNow + 60'000'000'000LL);
    limits = {};
    limits.max_idempotency_key_bytes = command.idempotency_key().size();
    EXPECT_TRUE(validate(command, command_binding, limits));
    command.set_idempotency_key("intentx");
    expectCode(validate(command, command_binding, limits),
               runtime::ProtocolValidationCode::InvalidIdentifier,
               "idempotency_key");
}

TEST(ProtocolValidationTest, EnforcesTypeSpecificCrossFields) {
    auto binding = TopicBinding{"kCommand", "test.command", MessageType::Command,
                                CatalogPayloadRole::CommandInput, "test.Command", 1,
                                4096, ProtocolPayloadValidatorKind::None};
    auto command = validEvent("test.command");
    command.set_message_type(wire::COMMAND);
    command.set_payload_role(wire::COMMAND_INPUT);
    command.set_payload_type("test.Command");
    command.set_priority(1);
    (*command.mutable_headers())["industrial_bus.message_class"] = "Command";
    command.set_destination_service("worker");
    command.set_idempotency_key("intent-1");
    command.set_deadline_unix_ns(kNow + std::chrono::hours(1).count() *
                                            60LL * 60LL * 1'000'000'000LL /
                                            std::chrono::hours(1).count());
    EXPECT_TRUE(validate(command, binding));

    auto invalid = command;
    invalid.clear_destination_service();
    expectCode(validate(invalid, binding),
               runtime::ProtocolValidationCode::MissingField,
               "destination_service");
    invalid = command;
    invalid.clear_idempotency_key();
    expectCode(validate(invalid, binding),
               runtime::ProtocolValidationCode::MissingField,
               "idempotency_key");
    invalid = command;
    invalid.set_deadline_unix_ns(kNow + std::chrono::hours(24).count() *
                                            60LL * 60LL * 1'000'000'000LL + 1);
    expectCode(validate(invalid, binding),
               runtime::ProtocolValidationCode::InvalidDeadline,
               "deadline_unix_ns");

    auto event = validEvent();
    auto event_binding = eventBinding();
    event.set_idempotency_key("not-allowed");
    expectCode(validate(event, event_binding),
               runtime::ProtocolValidationCode::InvalidSemantics,
               "idempotency_key");

    auto reply_binding = TopicBinding{
        "kReply", "test.reply", MessageType::Reply,
        CatalogPayloadRole::ResponseOutput, "test.Reply", 1, 4096,
        ProtocolPayloadValidatorKind::None};
    auto reply = validEvent("test.reply");
    reply.set_message_type(wire::REPLY);
    reply.set_payload_role(wire::RESPONSE_OUTPUT);
    reply.set_payload_type("test.Reply");
    reply.set_priority(0);
    (*reply.mutable_headers())["industrial_bus.message_class"] = "Reply";
    reply.set_causation_id("request-id");
    expectCode(validate(reply, reply_binding),
               runtime::ProtocolValidationCode::MissingField,
               "destination_service");
    reply.set_destination_service("caller");
    EXPECT_TRUE(validate(reply, reply_binding));
    reply.clear_causation_id();
    expectCode(validate(reply, reply_binding),
               runtime::ProtocolValidationCode::MissingField, "causation_id");
}

TEST(ProtocolValidationTest, ControlsReservedHeadersAndLegacyFallback) {
    auto binding = eventBinding();
    auto envelope = validEvent();
    EXPECT_TRUE(validate(envelope, binding));

    (*envelope.mutable_headers())["industrial_bus.unregistered"] = "x";
    expectCode(validate(envelope, binding),
               runtime::ProtocolValidationCode::ReservedHeader,
               "industrial_bus.unregistered");

    envelope = validEvent();
    (*envelope.mutable_headers())["authorization"] = "secret";
    expectCode(validate(envelope, binding),
               runtime::ProtocolValidationCode::ReservedHeader,
               "authorization");

    envelope = validEvent();
    envelope.mutable_headers()->clear();
    EXPECT_TRUE(validate(envelope, binding));
    envelope.set_topic("test.unknown");
    expectCode(validate(envelope, binding),
               runtime::ProtocolValidationCode::UnknownTopic, "topic");
}

TEST(ProtocolValidationTest, RequiresAnExactCatalogBindingButKeepsPayloadOpaque) {
    auto binding = eventBinding();
    auto envelope = validEvent();
    envelope.set_payload("not-a-protobuf-payload");
    EXPECT_TRUE(validate(envelope, binding));

    auto invalid = envelope;
    invalid.set_payload_type("test.Other");
    expectCode(validate(invalid, binding),
               runtime::ProtocolValidationCode::BindingMismatch, "binding");
    invalid = envelope;
    invalid.set_schema_version(2);
    expectCode(validate(invalid, binding),
               runtime::ProtocolValidationCode::BindingMismatch, "binding");
    invalid = envelope;
    invalid.set_message_type(wire::LOG);
    (*invalid.mutable_headers())["industrial_bus.message_class"] = "WarnLog";
    expectCode(validate(invalid, binding),
               runtime::ProtocolValidationCode::BindingMismatch, "binding");
}

TEST(ProtocolValidationTest, RunsOnlyRegisteredProtocolPayloadValidators) {
    auto envelope = validEvent("system.operation.progress");
    envelope.set_message_type(wire::PROGRESS);
    envelope.set_payload_role(wire::EVENT_FACT);
    envelope.set_payload_type("industrial_bus.operation.v1.ProgressEvent");
    envelope.set_operation_id("operation-envelope");
    (*envelope.mutable_headers())["industrial_bus.message_class"] = "Progress";
    operation::v1::ProgressEvent progress;
    progress.set_operation_id("operation-payload");
    ASSERT_TRUE(progress.SerializeToString(envelope.mutable_payload()));

    const auto result = runtime::productionProtocolValidator().validate(
        serialize(envelope), kNow);
    expectCode(result, runtime::ProtocolValidationCode::ProtocolPayloadInvalid,
               "payload.operation_id");
}

TEST(ProtocolValidationTest, RejectsUnknownOutboundBindingBeforeTransport) {
    client::OutboundMessage outbound;
    outbound.message_type = MessageType::Event;
    outbound.message_class = MessageClass::Event;
    outbound.priority = QosPriority::Normal;
    outbound.payload_role = client::PayloadRole::EventData;
    outbound.message_id = "outbound-message";
    outbound.correlation_id = "outbound-correlation";
    outbound.topic = "unregistered.application.event";
    outbound.payload_type = "application.Event";
    outbound.payload = {'x'};
    const auto encoded = runtime::encodeOutbound(
        outbound, runtime::SourceIdentity{"node", "publisher", "instance"});
    expectCode(encoded,
               runtime::ProtocolValidationCode::UnknownTopic, "topic");
}

}  // namespace
