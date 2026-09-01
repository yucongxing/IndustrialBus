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
    const google::protobuf::MessageLite& payload,
    const std::string& auth_token,
    const std::string& message_id = {},
    envelope::MessageType message_type = envelope::SYSTEM) {
    envelope::MessageEnvelope wire;
    wire.set_message_id(message_id.empty() ? generateUuidV4() : message_id);
    wire.set_correlation_id(generateUuidV4());
    wire.set_source_node(source.node_id);
    wire.set_source_service(source.service_name);
    wire.set_source_instance(source.instance_id);
    wire.set_topic(topic);
    wire.set_message_type(message_type);
    wire.set_payload_role(envelope::SYSTEM_PAYLOAD);
    wire.set_payload_type(payload_type);
    wire.set_protocol_version(1);
    wire.set_schema_version(1);
    wire.set_max_hops(1);
    if (!auth_token.empty()) {
        (*wire.mutable_headers())["authorization"] = auth_token;
    }
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
    std::unique_lock<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!options_.connector || options_.endpoint.empty() ||
        options_.service_name.empty()) {
        return Error::invalidArgument(
            "runtime adapter requires connector, endpoint and service name");
    }
    if (options_.heartbeat_interval < std::chrono::milliseconds::zero() ||
        (options_.heartbeat_interval > std::chrono::milliseconds::zero() &&
         options_.heartbeat_timeout < options_.heartbeat_interval * 2)) {
        return Error::invalidArgument(
            "heartbeat timeout must be at least twice its non-negative interval");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ConnectionState::Disconnected) {
            return Error::invalidArgument("runtime adapter is already started");
        }
        if (heartbeat_thread_.joinable()) {
            return Error::invalidArgument(
                "runtime adapter must be closed after heartbeat loss before restart");
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = ConnectionState::Connecting;
        registered_ = false;
        registration_error_.reset();
        heartbeat_stop_ = false;
        startup_complete_ = false;
        connected_notification_in_progress_ = false;
        disconnected_notification_pending_ = false;
        heartbeat_disconnect_callback_active_ = false;
        pending_heartbeats_.clear();
        heartbeat_order_.clear();
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
        lifecycle_lock.unlock();
        close();
        return registration;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!registration_cv_.wait_for(lock, options_.handshake_timeout,
                                   [this] {
                                       return registered_ || registration_error_.has_value();
                                   })) {
        lock.unlock();
        lifecycle_lock.unlock();
        close();
        return Error::timeout("CommHub registration handshake timed out");
    }
    if (registration_error_) {
        const auto error = *registration_error_;
        lock.unlock();
        lifecycle_lock.unlock();
        close();
        return error;
    }
    state_ = ConnectionState::Connected;
    ++generation_;
    last_hub_activity_ = std::chrono::steady_clock::now();
    auto handler = connection_handler_;
    const auto generation_value = generation_;
    connected_notification_in_progress_ = static_cast<bool>(handler);
    if (!handler) {
        startup_complete_ = true;
    }
    lock.unlock();
    if (options_.heartbeat_interval > std::chrono::milliseconds::zero()) {
        try {
            heartbeat_thread_ = std::thread([this]() noexcept {
                try {
                    heartbeatLoop();
                } catch (...) {
                    (void)disconnectFromHeartbeat(false);
                }
            });
        } catch (const std::exception& error) {
            lifecycle_lock.unlock();
            close();
            return Error::internal(std::string("failed to start heartbeat worker: ") +
                                   error.what());
        }
    }
    lifecycle_lock.unlock();
    if (handler) {
        try {
            handler(ConnectionState::Connected, generation_value);
        } catch (...) {
            // Client callbacks do not control the runtime lifecycle.
        }
    }
    ConnectionHandler disconnected_handler;
    std::uint64_t disconnected_generation = 0;
    bool notify_disconnected = false;
    {
        std::lock_guard<std::mutex> state_lock(mutex_);
        connected_notification_in_progress_ = false;
        startup_complete_ = true;
        if (disconnected_notification_pending_) {
            notify_disconnected = true;
            disconnected_notification_pending_ = false;
            disconnected_generation = pending_disconnected_generation_;
            try {
                disconnected_handler = connection_handler_;
            } catch (...) {
                notify_disconnected = false;
            }
        }
    }
    heartbeat_cv_.notify_all();
    if (notify_disconnected && disconnected_handler) {
        try {
            disconnected_handler(ConnectionState::Disconnected,
                                 disconnected_generation);
        } catch (...) {
            // Deferred lifecycle notifications are isolated from start().
        }
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

ServiceInfo TransportRuntimeAdapter::localService() const {
    return {options_.service_name, options_.instance_id, true};
}

bool TransportRuntimeAdapter::flush(std::chrono::milliseconds) {
    return true;
}

void TransportRuntimeAdapter::close() noexcept {
    std::unique_lock<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    std::shared_ptr<transport::IConnection> connection;
    ConnectionHandler handler;
    std::thread heartbeat;
    std::uint64_t generation_value = 0;
    bool notify_disconnected = false;
    bool heartbeat_callback_active = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ConnectionState::Disconnected && !connection_ &&
            !heartbeat_thread_.joinable()) {
            return;
        }
        notify_disconnected = state_ != ConnectionState::Disconnected &&
                              state_ != ConnectionState::ShuttingDown;
        state_ = ConnectionState::ShuttingDown;
        registered_ = false;
        heartbeat_stop_ = true;
        connection = std::move(connection_);
        try {
            handler = connection_handler_;
        } catch (...) {
            // A throwing callback copy must not violate close()'s noexcept contract.
        }
        generation_value = generation_;
        heartbeat = std::move(heartbeat_thread_);
        heartbeat_callback_active = heartbeat_disconnect_callback_active_;
        if (notify_disconnected && connected_notification_in_progress_) {
            disconnected_notification_pending_ = true;
            pending_disconnected_generation_ = generation_value;
            notify_disconnected = false;
        }
    }
    heartbeat_cv_.notify_all();
    if (heartbeat.joinable()) {
        if (heartbeat.get_id() == std::this_thread::get_id() ||
            heartbeat_callback_active) {
            heartbeat.detach();
        } else {
            heartbeat.join();
        }
    }
    if (connection) {
        try {
            connection->setReceiveHandler({});
        } catch (...) {
            // Best-effort transport cleanup during shutdown.
        }
        try {
            connection->close();
        } catch (...) {
            // Best-effort transport cleanup during shutdown.
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = ConnectionState::Disconnected;
        pending_heartbeats_.clear();
        heartbeat_order_.clear();
    }
    lifecycle_lock.unlock();
    if (notify_disconnected && handler) {
        try {
            handler(ConnectionState::Disconnected, generation_value);
        } catch (...) {
            // close() is noexcept and client callbacks are isolated from it.
        }
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
    const auto registration_message_id = generateUuidV4();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registration_message_id_ = registration_message_id;
    }
    const auto frame = serializeSystemEnvelope(
        source, "system.service.register",
        "industrial_bus.system.v1.ServiceRegistration", registration,
        options_.auth_token, registration_message_id);
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
        "industrial_bus.system.v1.SubscriptionUpdate", update,
        options_.auth_token);
    const auto sent = connection->send(transport::ByteView(frame));
    return sent ? Result<void>::success() : Result<void>::failure(fromTransport(sent.error()));
}

Result<void> TransportRuntimeAdapter::sendHeartbeat(
    const std::shared_ptr<transport::IConnection>& connection,
    const std::string& message_id,
    std::uint64_t sequence) {
    system_proto::Heartbeat heartbeat;
    heartbeat.set_sent_at_unix_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
    heartbeat.set_sequence(sequence);
    const SourceIdentity source{options_.node_id, options_.service_name,
                                options_.instance_id};
    const auto frame = serializeSystemEnvelope(
        source, "system.heartbeat", "industrial_bus.system.v1.Heartbeat",
        heartbeat, options_.auth_token, message_id, envelope::HEARTBEAT);
    const auto sent = connection->send(transport::ByteView(frame));
    return sent ? Result<void>::success()
                : Result<void>::failure(fromTransport(sent.error()));
}

void TransportRuntimeAdapter::heartbeatLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    heartbeat_cv_.wait(lock, [this] {
        return heartbeat_stop_ || startup_complete_;
    });
    while (!heartbeat_stop_) {
        if (heartbeat_cv_.wait_for(lock, options_.heartbeat_interval,
                                   [this] { return heartbeat_stop_; })) {
            return;
        }
        if (state_ != ConnectionState::Connected || !connection_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_hub_activity_ >= options_.heartbeat_timeout) {
            lock.unlock();
            if (!disconnectFromHeartbeat(true)) {
                lock.lock();
                continue;
            }
            return;
        }

        const auto message_id = generateUuidV4();
        const auto sequence = next_heartbeat_sequence_++;
        while (heartbeat_order_.size() >= 16) {
            pending_heartbeats_.erase(heartbeat_order_.front());
            heartbeat_order_.pop_front();
        }
        pending_heartbeats_[message_id] = sequence;
        heartbeat_order_.push_back(message_id);
        auto connection = connection_;
        lock.unlock();
        (void)sendHeartbeat(connection, message_id, sequence);
        lock.lock();
    }
}

bool TransportRuntimeAdapter::disconnectFromHeartbeat(
    bool require_timeout) noexcept {
    std::shared_ptr<transport::IConnection> connection;
    ConnectionHandler handler;
    std::uint64_t generation_value = 0;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (heartbeat_stop_ || state_ != ConnectionState::Connected) {
            return false;
        }
        if (require_timeout &&
            std::chrono::steady_clock::now() - last_hub_activity_ <
                options_.heartbeat_timeout) {
            return false;
        }
        state_ = ConnectionState::Disconnected;
        registered_ = false;
        heartbeat_stop_ = true;
        connection = std::move(connection_);
        generation_value = generation_;
        try {
            handler = connection_handler_;
        } catch (...) {
            // The state transition and transport cleanup still take precedence.
        }
        if (handler && connected_notification_in_progress_) {
            disconnected_notification_pending_ = true;
            pending_disconnected_generation_ = generation_value;
            handler = {};
        }
        heartbeat_disconnect_callback_active_ = static_cast<bool>(handler);
    } catch (...) {
        return false;
    }

    if (connection) {
        try {
            connection->setReceiveHandler({});
        } catch (...) {
            // Best-effort cleanup from the worker thread.
        }
        try {
            connection->close();
        } catch (...) {
            // Best-effort cleanup from the worker thread.
        }
    }
    if (handler) {
        try {
            handler(ConnectionState::Disconnected, generation_value);
        } catch (...) {
            // Client callbacks must never escape a std::thread entry point.
        }
    }
    return true;
}

void TransportRuntimeAdapter::receive(transport::ByteBuffer frame) {
    envelope::MessageEnvelope wire;
    if (!wire.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
        return;
    }
    std::string registration_message_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registration_message_id = registration_message_id_;
    }
    if (wire.topic() == "system.service.register" &&
        wire.payload_type() == "industrial_bus.system.v1.ServiceRegistrationAck") {
        system_proto::ServiceRegistrationAck ack;
        if (ack.ParseFromString(wire.payload()) &&
            wire.causation_id() == registration_message_id &&
            (options_.expected_hub_instance_id.empty() ||
             ack.hub_instance_id() == options_.expected_hub_instance_id) &&
            ack.negotiated_protocol_version() ==
                options_.expected_protocol_version) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                registered_ = true;
                last_hub_activity_ = std::chrono::steady_clock::now();
            }
            registration_cv_.notify_all();
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                registration_error_ =
                    Error::protocol("CommHub registration Ack does not match discovery");
            }
            registration_cv_.notify_all();
        }
        return;
    }

    if (wire.topic() == "system.heartbeat" &&
        wire.payload_type() == "industrial_bus.system.v1.HeartbeatAck") {
        system_proto::HeartbeatAck ack;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto pending = pending_heartbeats_.find(wire.causation_id());
        if (wire.message_type() == envelope::REPLY &&
            wire.payload_role() == envelope::SYSTEM_PAYLOAD &&
            ack.ParseFromString(wire.payload()) &&
            pending != pending_heartbeats_.end() &&
            pending->second == ack.sequence() &&
            (options_.expected_hub_instance_id.empty() ||
             ack.hub_instance_id() == options_.expected_hub_instance_id)) {
            pending_heartbeats_.erase(pending);
            last_hub_activity_ = std::chrono::steady_clock::now();
        }
        return;
    }

    auto decoded = decodeInbound(frame);
    if (!decoded) {
        return;
    }
    if (wire.causation_id() == registration_message_id &&
        decoded.value().has_error) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            registration_error_ = decoded.value().error;
        }
        registration_cv_.notify_all();
        return;
    }
    ReceiveHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_hub_activity_ = std::chrono::steady_clock::now();
        handler = receive_handler_;
    }
    if (handler) {
        try {
            handler(std::move(decoded).value());
        } catch (...) {
            // Client receive callbacks are isolated from transport threads.
        }
    }
}

}  // namespace industrial_bus::runtime
