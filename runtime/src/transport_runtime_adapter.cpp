#include "industrial_bus/runtime/transport_runtime_adapter.h"

#include "industrial_bus/ids.h"
#include "industrial_bus/runtime/envelope_codec.h"

#include "envelope.pb.h"
#include "system.pb.h"

#include <chrono>
#include <string>
#include <utility>

namespace industrial_bus::runtime {
namespace {

namespace envelope = industrial_bus::protocol::v1;
namespace system_proto = industrial_bus::system::v1;

transport::ByteBuffer serializeSystemEnvelope(
    const SourceIdentity& source,
    const std::string& topic,
    const std::string& payload_type,
    const google::protobuf::MessageLite& payload) {
    envelope::MessageEnvelope wire;
    wire.set_message_id(generateUuidV4());
    wire.set_correlation_id(generateUuidV4());
    wire.set_source_node(source.node_id);
    wire.set_source_service(source.service_name);
    wire.set_source_instance(source.instance_id);
    wire.set_topic(topic);
    wire.set_message_type(envelope::SYSTEM);
    wire.set_payload_role(envelope::SYSTEM_PAYLOAD);
    wire.set_payload_type(payload_type);
    wire.set_protocol_version(1);
    wire.set_schema_version(1);
    wire.set_max_hops(1);
    payload.SerializeToString(wire.mutable_payload());
    std::string bytes;
    wire.SerializeToString(&bytes);
    return transport::ByteBuffer(bytes.begin(), bytes.end());
}

Error fromTransport(const transport::Error& error) {
    if (error.code == transport::ErrorCode::resource_exhausted) {
        return Error::resourceExhausted(error.message);
    }
    if (error.code == transport::ErrorCode::invalid_argument) {
        return Error::invalidArgument(error.message);
    }
    return Error::connectionLost(error.message);
}

}  // namespace

TransportRuntimeAdapter::TransportRuntimeAdapter(TransportRuntimeOptions options)
    : options_(std::move(options)) {
    if (options_.instance_id.empty()) {
        options_.instance_id = generateUuidV4();
    }
}

TransportRuntimeAdapter::~TransportRuntimeAdapter() { close(); }

Result<void> TransportRuntimeAdapter::start() {
    if (!options_.connector || options_.endpoint.empty() ||
        options_.service_name.empty()) {
        return Error::invalidArgument(
            "runtime adapter requires connector, endpoint and service name");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ConnectionState::Disconnected) {
            return Error::invalidArgument("runtime adapter is already started");
        }
        state_ = ConnectionState::Connecting;
    }

    auto connected = options_.connector->connect(options_.endpoint).get();
    if (!connected) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = ConnectionState::Disconnected;
        return fromTransport(connected.error());
    }
    connection_ = std::move(connected).value();
    connection_->setReceiveHandler(
        [this](transport::ByteBuffer frame) { receive(std::move(frame)); });

    auto registration = sendRegistration();
    if (!registration) {
        close();
        return registration;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!registration_cv_.wait_for(lock, options_.handshake_timeout,
                                   [this] { return registered_; })) {
        lock.unlock();
        close();
        return Error::timeout("CommHub registration handshake timed out");
    }
    state_ = ConnectionState::Connected;
    ++generation_;
    auto handler = connection_handler_;
    const auto generation_value = generation_;
    lock.unlock();
    if (handler) {
        handler(ConnectionState::Connected, generation_value);
    }
    return Result<void>::success();
}

Result<void> TransportRuntimeAdapter::send(client::OutboundMessage message) {
    std::shared_ptr<transport::IConnection> connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ConnectionState::Connected || !connection_) {
            return Error::connectionLost("runtime adapter is not connected");
        }
        connection = connection_;
    }
    const SourceIdentity source{options_.node_id, options_.service_name,
                                options_.instance_id};
    auto encoded = encodeOutbound(message, source);
    if (!encoded) {
        return encoded.error();
    }
    auto sent = connection->send(transport::ByteView(encoded.value()));
    return sent ? Result<void>::success() : Result<void>::failure(fromTransport(sent.error()));
}

Result<void> TransportRuntimeAdapter::subscribe(const std::string& topic) {
    return sendSubscription(topic, true);
}

Result<void> TransportRuntimeAdapter::unsubscribe(const std::string& topic) {
    return sendSubscription(topic, false);
}

void TransportRuntimeAdapter::setReceiveHandler(ReceiveHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    receive_handler_ = std::move(handler);
}

void TransportRuntimeAdapter::setConnectionHandler(ConnectionHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    connection_handler_ = std::move(handler);
}

ConnectionState TransportRuntimeAdapter::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::uint64_t TransportRuntimeAdapter::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

ServiceSnapshot TransportRuntimeAdapter::services() const {
    return {{options_.service_name, options_.instance_id, true}};
}

bool TransportRuntimeAdapter::flush(std::chrono::milliseconds) {
    return true;
}

void TransportRuntimeAdapter::close() noexcept {
    std::shared_ptr<transport::IConnection> connection;
    ConnectionHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ConnectionState::Disconnected && !connection_) {
            return;
        }
        state_ = ConnectionState::ShuttingDown;
        registered_ = false;
        connection = std::move(connection_);
        handler = connection_handler_;
    }
    if (connection) {
        connection->close();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = ConnectionState::Disconnected;
    }
    if (handler) {
        handler(ConnectionState::Disconnected, generation());
    }
}

Result<void> TransportRuntimeAdapter::sendRegistration() {
    system_proto::ServiceRegistration registration;
    registration.set_node_id(options_.node_id);
    registration.set_service_name(options_.service_name);
    registration.set_instance_id(options_.instance_id);
    registration.set_version(options_.service_version);
    registration.set_instance_mode(system_proto::SINGLETON_PER_USER);
    const SourceIdentity source{options_.node_id, options_.service_name,
                                options_.instance_id};
    const auto frame = serializeSystemEnvelope(
        source, "system.service.register",
        "industrial_bus.system.v1.ServiceRegistration", registration);
    auto sent = connection_->send(transport::ByteView(frame));
    return sent ? Result<void>::success() : Result<void>::failure(fromTransport(sent.error()));
}

Result<void> TransportRuntimeAdapter::sendSubscription(const std::string& topic,
                                                       bool subscribe_value) {
    if (topic.empty()) {
        return Error::invalidArgument("subscription topic is required");
    }
    std::shared_ptr<transport::IConnection> connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ConnectionState::Connected || !connection_) {
            return Error::connectionLost("runtime adapter is not connected");
        }
        connection = connection_;
    }
    system_proto::SubscriptionUpdate update;
    update.set_topic(topic);
    update.set_subscribe(subscribe_value);
    const SourceIdentity source{options_.node_id, options_.service_name,
                                options_.instance_id};
    const auto frame = serializeSystemEnvelope(
        source, "system.subscription.update",
        "industrial_bus.system.v1.SubscriptionUpdate", update);
    const auto sent = connection->send(transport::ByteView(frame));
    return sent ? Result<void>::success() : Result<void>::failure(fromTransport(sent.error()));
}

void TransportRuntimeAdapter::receive(transport::ByteBuffer frame) {
    envelope::MessageEnvelope wire;
    if (!wire.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
        return;
    }
    if (wire.topic() == "system.service.register" &&
        wire.payload_type() == "industrial_bus.system.v1.ServiceRegistrationAck") {
        system_proto::ServiceRegistrationAck ack;
        if (ack.ParseFromString(wire.payload())) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                registered_ = true;
            }
            registration_cv_.notify_all();
        }
        return;
    }

    auto decoded = decodeInbound(frame);
    if (!decoded) {
        return;
    }
    ReceiveHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = receive_handler_;
    }
    if (handler) {
        handler(std::move(decoded).value());
    }
}

}  // namespace industrial_bus::runtime
