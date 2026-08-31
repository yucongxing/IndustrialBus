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

std::int64_t unixNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

envelope::MessageType toWire(client::MessageKind kind) {
    switch (kind) {
        case client::MessageKind::Event:
        case client::MessageKind::Operation: return envelope::EVENT;
        case client::MessageKind::Command: return envelope::COMMAND;
        case client::MessageKind::Request: return envelope::REQUEST;
        case client::MessageKind::Reply: return envelope::REPLY;
    }
    return envelope::MESSAGE_TYPE_UNSPECIFIED;
}

client::MessageKind fromWire(envelope::MessageType kind, const std::string& topic) {
    if (topic == "system.operation.event") {
        return client::MessageKind::Operation;
    }
    switch (kind) {
        case envelope::COMMAND: return client::MessageKind::Command;
        case envelope::REQUEST: return client::MessageKind::Request;
        case envelope::REPLY:
        case envelope::ERROR: return client::MessageKind::Reply;
        case envelope::EVENT:
        case envelope::PROGRESS:
        case envelope::LOG:
        case envelope::HEARTBEAT:
        case envelope::SYSTEM:
        case envelope::MESSAGE_TYPE_UNSPECIFIED:
        default: return client::MessageKind::Event;
    }
    return client::MessageKind::Event;
}

envelope::PayloadRole toWire(client::PayloadRole role) {
    switch (role) {
        case client::PayloadRole::EventData:
        case client::PayloadRole::OperationData: return envelope::EVENT_FACT;
        case client::PayloadRole::CommandData: return envelope::COMMAND_INPUT;
        case client::PayloadRole::RequestData: return envelope::REQUEST_INPUT;
        case client::PayloadRole::ResponseData: return envelope::RESPONSE_OUTPUT;
        case client::PayloadRole::CommandAck: return envelope::COMMAND_ACK;
        case client::PayloadRole::ErrorDetail: return envelope::ERROR_DETAIL;
    }
    return envelope::PAYLOAD_ROLE_UNSPECIFIED;
}

client::PayloadRole fromWire(envelope::PayloadRole role) {
    switch (role) {
        case envelope::COMMAND_INPUT: return client::PayloadRole::CommandData;
        case envelope::COMMAND_ACK: return client::PayloadRole::CommandAck;
        case envelope::REQUEST_INPUT: return client::PayloadRole::RequestData;
        case envelope::RESPONSE_OUTPUT: return client::PayloadRole::ResponseData;
        case envelope::ERROR_DETAIL: return client::PayloadRole::ErrorDetail;
        case envelope::EVENT_FACT: return client::PayloadRole::EventData;
        case envelope::SYSTEM_PAYLOAD:
        case envelope::PAYLOAD_ROLE_UNSPECIFIED:
        default: return client::PayloadRole::EventData;
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

Result<envelope::MessageEnvelope> parseEnvelope(const transport::ByteBuffer& frame) {
    envelope::MessageEnvelope message;
    if (frame.size() > static_cast<std::size_t>(INT32_MAX) ||
        !message.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
        return Error::protocol("invalid MessageEnvelope frame");
    }
    if (message.message_id().empty() || message.topic().empty()) {
        return Error::protocol("MessageEnvelope requires message_id and topic");
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
    wire.set_message_type(toWire(message.kind));
    wire.set_payload_role(toWire(message.payload_role));
    wire.set_payload_type(message.payload_type);
    wire.set_idempotency_key(message.idempotency_key);
    wire.set_deadline_unix_ns(message.deadline_unix_ns);
    wire.set_timestamp_unix_ns(unixNowNs());
    wire.set_protocol_version(1);
    wire.set_schema_version(1);
    wire.set_max_hops(1);

    std::string payload(message.payload.begin(), message.payload.end());
    if (message.operation) {
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
    client::InboundMessage result;
    result.kind = fromWire(wire.message_type(), wire.topic());
    result.payload_role = fromWire(wire.payload_role());
    result.message_id = wire.message_id();
    result.correlation_id = wire.correlation_id();
    result.causation_id = wire.causation_id();
    result.operation_id = wire.operation_id();
    result.topic = wire.topic();
    result.payload_type = wire.payload_type();
    result.payload.assign(wire.payload().begin(), wire.payload().end());

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
    result.payload_role = routeRole(wire.payload_role());
    result.selection_policy = commhub::SelectionPolicy::prefer_local;
    result.priority = wire.priority();
    result.deadline_unix_ns = wire.deadline_unix_ns();
    result.wire_frame = frame;
    return result;
}

}  // namespace industrial_bus::runtime
