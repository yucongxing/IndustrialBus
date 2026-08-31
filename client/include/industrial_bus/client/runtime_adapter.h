#pragma once

#include "industrial_bus/client_options.h"
#include "industrial_bus/destination.h"
#include "industrial_bus/payload_codec.h"
#include "industrial_bus/operation.h"
#include "industrial_bus/result.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace industrial_bus::client {

enum class MessageKind {
    Event,
    Command,
    Request,
    Reply,
    Operation,
};

enum class PayloadRole {
    EventData,
    CommandData,
    RequestData,
    ResponseData,
    CommandAck,
    ErrorDetail,
    OperationData,
};

struct OutboundMessage {
    MessageKind kind{MessageKind::Event};
    PayloadRole payload_role{PayloadRole::EventData};
    std::string message_id;
    std::string correlation_id;
    std::string causation_id;
    std::string operation_id;
    Destination destination;
    std::string topic;
    std::string payload_type;
    ByteBuffer payload;
    std::string idempotency_key;
    std::int64_t deadline_unix_ns{0};
    std::shared_ptr<OperationUpdate> operation;
    std::shared_ptr<Error> error;
};

struct InboundMessage {
    MessageKind kind{MessageKind::Event};
    PayloadRole payload_role{PayloadRole::EventData};
    std::string message_id;
    std::string correlation_id;
    std::string causation_id;
    std::string operation_id;
    std::string source_service;
    std::string source_instance;
    std::string topic;
    std::string payload_type;
    ByteBuffer payload;
    std::string idempotency_key;
    std::int64_t deadline_unix_ns{0};
    bool has_error{false};
    Error error;
};

// This is the narrow seam between the Client Runtime and protocol/transport.
// Implementations own framing, handshake, retries and their I/O thread.
class IRuntimeAdapter {
public:
    using ReceiveHandler = std::function<void(InboundMessage)>;
    using ConnectionHandler = std::function<void(ConnectionState, std::uint64_t)>;

    virtual ~IRuntimeAdapter() = default;
    virtual Result<void> start() = 0;
    virtual Result<void> send(OutboundMessage message) = 0;
    virtual Result<void> subscribe(const std::string&) { return Result<void>::success(); }
    virtual Result<void> unsubscribe(const std::string&) { return Result<void>::success(); }
    virtual void setReceiveHandler(ReceiveHandler handler) = 0;
    virtual void setConnectionHandler(ConnectionHandler handler) = 0;
    virtual ConnectionState state() const = 0;
    virtual std::uint64_t generation() const = 0;
    virtual ServiceSnapshot services() const = 0;
    virtual bool flush(std::chrono::milliseconds timeout) = 0;
    virtual void close() noexcept = 0;
};

}  // namespace industrial_bus::client
