#include "industrial_bus/runtime/envelope_codec.h"

#include "envelope.pb.h"
#include "operation.pb.h"
#include "system.pb.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace industrial_bus::runtime {
namespace {

namespace envelope = industrial_bus::protocol::v1;
namespace operation_proto = industrial_bus::operation::v1;
namespace system_proto = industrial_bus::system::v1;

constexpr char kMessageClassHeader[] = "industrial_bus.message_class";

std::int64_t unixNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

envelope::MessageType toWire(MessageType type) {
    switch (type) {
        case MessageType::Command: return envelope::COMMAND;
        case MessageType::Event: return envelope::EVENT;
        case MessageType::Request: return envelope::REQUEST;
        case MessageType::Reply: return envelope::REPLY;
        case MessageType::Progress: return envelope::PROGRESS;
        case MessageType::Error: return envelope::ERROR;
        case MessageType::Log: return envelope::LOG;
        case MessageType::Heartbeat: return envelope::HEARTBEAT;
        case MessageType::System: return envelope::SYSTEM;
    }
    return envelope::MESSAGE_TYPE_UNSPECIFIED;
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
            return Error::protocol("unsupported or unspecified MessageType");
    }
}

envelope::PayloadRole toWire(client::PayloadRole role) {
    switch (role) {
        case client::PayloadRole::Unspecified:
            return envelope::PAYLOAD_ROLE_UNSPECIFIED;
        case client::PayloadRole::EventData:
        case client::PayloadRole::OperationData: return envelope::EVENT_FACT;
        case client::PayloadRole::CommandData: return envelope::COMMAND_INPUT;
        case client::PayloadRole::RequestData: return envelope::REQUEST_INPUT;
        case client::PayloadRole::ResponseData: return envelope::RESPONSE_OUTPUT;
        case client::PayloadRole::CommandAck: return envelope::COMMAND_ACK;
        case client::PayloadRole::ErrorDetail: return envelope::ERROR_DETAIL;
        case client::PayloadRole::SystemData: return envelope::SYSTEM_PAYLOAD;
    }
    return envelope::PAYLOAD_ROLE_UNSPECIFIED;
}

client::PayloadRole fromWire(envelope::PayloadRole role) {
    switch (role) {
        case envelope::PAYLOAD_ROLE_UNSPECIFIED:
            return client::PayloadRole::Unspecified;
        case envelope::COMMAND_INPUT: return client::PayloadRole::CommandData;
        case envelope::COMMAND_ACK: return client::PayloadRole::CommandAck;
        case envelope::REQUEST_INPUT: return client::PayloadRole::RequestData;
        case envelope::RESPONSE_OUTPUT: return client::PayloadRole::ResponseData;
        case envelope::ERROR_DETAIL: return client::PayloadRole::ErrorDetail;
        case envelope::EVENT_FACT: return client::PayloadRole::EventData;
        case envelope::SYSTEM_PAYLOAD: return client::PayloadRole::SystemData;
        default: return client::PayloadRole::Unspecified;
    }
    return client::PayloadRole::EventData;
}

operation_proto::OperationState toWire(OperationState state) {
    switch (state) {
        case OperationState::Created: return operation_proto::CREATED;
        case OperationState::Running: return operation_proto::RUNNING;
        case OperationState::Cancelling: return operation_proto::CANCELLING;
        case OperationState::Completed: return operation_proto::COMPLETED;
        case OperationState::Failed: return operation_proto::FAILED;
        case OperationState::Cancelled: return operation_proto::CANCELLED;
        case OperationState::Unspecified: return operation_proto::OPERATION_STATE_UNSPECIFIED;
    }
    return operation_proto::OPERATION_STATE_UNSPECIFIED;
}

void fillError(const Error& source, system_proto::Error& target) {
    target.set_category(static_cast<system_proto::ErrorCategory>(source.category));
    target.set_code(source.code);
    target.set_message(source.message);
    target.set_retryable(source.retryable);
    target.set_operation_id(source.operation_id);
    for (const auto& item : source.context) {
        (*target.mutable_context())[item.first] = item.second;
    }
}

Error readError(const system_proto::Error& source) {
    Error result;
    result.category = static_cast<ErrorCategory>(source.category());
    result.code = source.code();
    result.message = source.message();
    result.retryable = source.retryable();
    result.operation_id = source.operation_id();
    result.context.insert(source.context().begin(), source.context().end());
    return result;
}

commhub::MessageKind routeKind(envelope::MessageType kind) {
    switch (kind) {
        case envelope::COMMAND: return commhub::MessageKind::command;
        case envelope::EVENT: return commhub::MessageKind::event;
        case envelope::REQUEST: return commhub::MessageKind::request;
        case envelope::REPLY: return commhub::MessageKind::reply;
        case envelope::PROGRESS: return commhub::MessageKind::progress;
        case envelope::ERROR: return commhub::MessageKind::error;
        case envelope::LOG: return commhub::MessageKind::log;
        case envelope::HEARTBEAT: return commhub::MessageKind::heartbeat;
        case envelope::SYSTEM:
        case envelope::MESSAGE_TYPE_UNSPECIFIED:
        default: return commhub::MessageKind::system;
    }
    return commhub::MessageKind::system;
}

commhub::PayloadRole routeRole(envelope::PayloadRole role) {
    return static_cast<commhub::PayloadRole>(static_cast<int>(role));
}

bool roleMatches(MessageType type, envelope::PayloadRole role) {
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
        case MessageType::Log:
            return role == envelope::EVENT_FACT;
        case MessageType::Error: return role == envelope::ERROR_DETAIL;
        case MessageType::Heartbeat:
        case MessageType::System:
            return role == envelope::SYSTEM_PAYLOAD;
    }
    return false;
}

bool typeMatchesClass(MessageType type, MessageClass message_class) {
    return messageTypeFor(message_class) == type ||
           (type == MessageType::Reply && message_class == MessageClass::Error);
}

MessageClass legacyClass(MessageType type,
                         envelope::PayloadRole role,
                         std::uint32_t priority) {
    switch (type) {
        case MessageType::Command: return MessageClass::Command;
        case MessageType::Event: return MessageClass::Event;
        case MessageType::Request: return MessageClass::Request;
        case MessageType::Reply:
            if (role == envelope::COMMAND_ACK) {
                return MessageClass::Accepted;
            }
            if (role == envelope::ERROR_DETAIL) {
                return MessageClass::Error;
            }
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

struct WireSemantics {
    MessageType message_type;
    MessageClass message_class;
    QosPriority priority;
};

Result<WireSemantics> readSemantics(const envelope::MessageEnvelope& wire) {
    auto message_type = fromWire(wire.message_type());
    if (!message_type) {
        return message_type.error();
    }
    if (!roleMatches(message_type.value(), wire.payload_role())) {
        return Error::protocol("MessageType and PayloadRole are inconsistent");
    }
    if (wire.priority() > static_cast<std::uint32_t>(QosPriority::BestEffort)) {
        return Error::protocol("Envelope priority is outside P0 through P3");
    }

    const auto header = wire.headers().find(kMessageClassHeader);
    if (header != wire.headers().end()) {
        const auto message_class = messageClassFromName(header->second);
        if (!message_class ||
            !typeMatchesClass(message_type.value(), *message_class) ||
            wire.priority() !=
                static_cast<std::uint32_t>(defaultPriority(*message_class))) {
            return Error::protocol(
                "MessageType, priority and semantic class are inconsistent");
        }
        return WireSemantics{message_type.value(), *message_class,
                             defaultPriority(*message_class)};
    }

    const auto message_class =
        legacyClass(message_type.value(), wire.payload_role(), wire.priority());
    const auto canonical_priority = defaultPriority(message_class);
    const auto supplied_priority = static_cast<QosPriority>(wire.priority());
    const bool accepted_legacy_default =
        supplied_priority == QosPriority::Critical &&
        (message_type.value() == MessageType::Command ||
         message_type.value() == MessageType::Request ||
         message_type.value() == MessageType::Event ||
         message_type.value() == MessageType::Heartbeat ||
         message_type.value() == MessageType::System);
    if (supplied_priority != canonical_priority && !accepted_legacy_default) {
        return Error::protocol("legacy Envelope priority is inconsistent");
    }
    return WireSemantics{message_type.value(), message_class, canonical_priority};
}

Result<envelope::MessageEnvelope> parseEnvelope(const transport::ByteBuffer& frame) {
    envelope::MessageEnvelope message;
    if (frame.size() > static_cast<std::size_t>(INT32_MAX) ||
        !message.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
        return Error::protocol("invalid MessageEnvelope frame");
    }
    if (message.message_id().empty() || message.topic().empty()) {
        return Error::protocol("MessageEnvelope requires message_id and topic");
    }
    const auto semantics = readSemantics(message);
    if (!semantics) {
        return semantics.error();
    }
    if (message.topic() == "system.operation.progress") {
        if (semantics.value().message_class != MessageClass::Progress ||
            message.payload_type() !=
                operation_proto::ProgressEvent::descriptor()->full_name()) {
            return Error::protocol(
                "operation progress requires the canonical ProgressEvent contract");
        }
        operation_proto::ProgressEvent progress;
        if (!progress.ParseFromString(message.payload()) ||
            message.operation_id().empty() ||
            progress.operation_id() != message.operation_id()) {
            return Error::protocol(
                "ProgressEvent operation_id must match its Envelope");
        }
    }
    return message;
}

}  // namespace

Result<transport::ByteBuffer> encodeOutbound(const client::OutboundMessage& message,
                                             const SourceIdentity& source) {
    if (message.message_id.empty() || message.topic.empty() ||
        message.payload_type.empty() || source.service_name.empty() ||
        source.instance_id.empty()) {
        return Error::invalidArgument("outbound envelope is missing required metadata");
    }
    if (!typeMatchesClass(message.message_type, message.message_class) ||
        message.priority != defaultPriority(message.message_class)) {
        return Error::protocol(
            "outbound MessageType, priority and semantic class are inconsistent");
    }

    envelope::MessageEnvelope wire;
    wire.set_message_id(message.message_id);
    wire.set_correlation_id(message.correlation_id);
    wire.set_causation_id(message.causation_id);
    wire.set_operation_id(message.operation_id);
    wire.set_source_node(source.node_id);
    wire.set_source_service(source.service_name);
    wire.set_source_instance(source.instance_id);
    wire.set_destination_service(message.destination.service_name);
    wire.set_destination_instance(message.destination.instance_id);
    wire.set_topic(message.topic);
    wire.set_message_type(toWire(message.message_type));
    wire.set_payload_role(toWire(message.payload_role));
    wire.set_payload_type(message.payload_type);
    wire.set_idempotency_key(message.idempotency_key);
    wire.set_deadline_unix_ns(message.deadline_unix_ns);
    wire.set_priority(static_cast<std::uint32_t>(message.priority));
    wire.set_timestamp_unix_ns(unixNowNs());
    wire.set_protocol_version(1);
    wire.set_schema_version(1);
    wire.set_max_hops(1);
    (*wire.mutable_headers())[kMessageClassHeader] =
        std::string(messageClassName(message.message_class));

    std::string payload(message.payload.begin(), message.payload.end());
    if (message.error) {
        system_proto::Error error;
        fillError(*message.error, error);
        if (!error.SerializeToString(&payload)) {
            return Error::protocol("failed to serialize Error reply");
        }
        wire.set_payload_role(envelope::ERROR_DETAIL);
        wire.set_payload_type("industrial_bus.system.v1.Error");
    } else if (message.operation) {
        operation_proto::OperationEvent event;
        event.set_operation_id(message.operation->operation_id);
        event.set_operation_type(message.operation->operation_type);
        event.set_state(toWire(message.operation->state));
        event.set_stage(message.operation->stage);
        if (message.operation->stage_progress) {
            event.set_stage_progress(*message.operation->stage_progress);
        }
        if (message.operation->overall_progress) {
            event.set_overall_progress(*message.operation->overall_progress);
        }
        if (message.operation->error) {
            fillError(*message.operation->error, *event.mutable_error());
        }
        if (!event.SerializeToString(&payload)) {
            return Error::protocol("failed to serialize OperationEvent");
        }
    }
    wire.set_payload(std::move(payload));

    std::string serialized;
    if (!wire.SerializeToString(&serialized)) {
        return Error::protocol("failed to serialize MessageEnvelope");
    }
    return transport::ByteBuffer(serialized.begin(), serialized.end());
}

Result<client::InboundMessage> decodeInbound(const transport::ByteBuffer& frame) {
    auto parsed = parseEnvelope(frame);
    if (!parsed) {
        return parsed.error();
    }
    const auto& wire = parsed.value();
    const auto semantics = readSemantics(wire);
    if (!semantics) {
        return semantics.error();
    }
    client::InboundMessage result;
    result.message_type = semantics.value().message_type;
    result.message_class = semantics.value().message_class;
    result.priority = semantics.value().priority;
    result.payload_role = fromWire(wire.payload_role());
    result.message_id = wire.message_id();
    result.correlation_id = wire.correlation_id();
    result.causation_id = wire.causation_id();
    result.operation_id = wire.operation_id();
    result.source_service = wire.source_service();
    result.source_instance = wire.source_instance();
    result.topic = wire.topic();
    result.payload_type = wire.payload_type();
    result.payload.assign(wire.payload().begin(), wire.payload().end());
    result.idempotency_key = wire.idempotency_key();
    result.deadline_unix_ns = wire.deadline_unix_ns();

    if (wire.payload_role() == envelope::ERROR_DETAIL) {
        system_proto::Error error;
        if (!error.ParseFromString(wire.payload())) {
            return Error::protocol("failed to parse Error reply");
        }
        result.has_error = true;
        result.error = readError(error);
    }
    return result;
}

Result<commhub::RouteMessage> decodeRouteMessage(const transport::ByteBuffer& frame) {
    auto parsed = parseEnvelope(frame);
    if (!parsed) {
        return parsed.error();
    }
    const auto& wire = parsed.value();
    const auto semantics = readSemantics(wire);
    if (!semantics) {
        return semantics.error();
    }
    commhub::RouteMessage result;
    result.message_id = wire.message_id();
    result.correlation_id = wire.correlation_id();
    result.causation_id = wire.causation_id();
    result.operation_id = wire.operation_id();
    result.source_node = wire.source_node();
    result.source_service = wire.source_service();
    result.source_instance = wire.source_instance();
    result.destination_node = wire.destination_node();
    result.destination_service = wire.destination_service();
    result.destination_instance = wire.destination_instance();
    result.topic = wire.topic();
    result.payload_type = wire.payload_type();
    result.idempotency_key = wire.idempotency_key();
    result.kind = routeKind(wire.message_type());
    result.message_class = semantics.value().message_class;
    result.payload_role = routeRole(wire.payload_role());
    result.selection_policy = commhub::SelectionPolicy::prefer_local;
    result.priority = static_cast<std::uint32_t>(semantics.value().priority);
    result.deadline_unix_ns = wire.deadline_unix_ns();
    result.wire_frame = frame;
    return result;
}

}  // namespace industrial_bus::runtime
