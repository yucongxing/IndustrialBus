#include "industrial_bus/runtime/commhub_server.h"

#include "industrial_bus/ids.h"
#include "industrial_bus/runtime/envelope_codec.h"

#include "envelope.pb.h"
#include "system.pb.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace industrial_bus::runtime {
namespace {

namespace envelope = industrial_bus::protocol::v1;
namespace system_proto = industrial_bus::system::v1;

std::int64_t unixNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

transport::ByteBuffer serializeReply(const envelope::MessageEnvelope& request,
                                     const std::string& payload_type,
                                     envelope::PayloadRole role,
                                     const google::protobuf::MessageLite& payload,
                                     const std::string& source_service,
                                     const std::string& source_instance) {
    envelope::MessageEnvelope reply;
    reply.set_message_id(generateUuidV4());
    reply.set_correlation_id(request.correlation_id());
    reply.set_causation_id(request.message_id());
    reply.set_operation_id(request.operation_id());
    reply.set_source_node("local");
    reply.set_source_service(source_service);
    reply.set_source_instance(source_instance);
    reply.set_destination_service(request.source_service());
    reply.set_destination_instance(request.source_instance());
    reply.set_topic(request.topic());
    reply.set_message_type(envelope::REPLY);
    reply.set_payload_role(role);
    reply.set_payload_type(payload_type);
    reply.set_timestamp_unix_ns(unixNowNs());
    reply.set_protocol_version(1);
    reply.set_schema_version(1);
    reply.set_max_hops(1);
    payload.SerializeToString(reply.mutable_payload());
    std::string bytes;
    reply.SerializeToString(&bytes);
    return transport::ByteBuffer(bytes.begin(), bytes.end());
}

system_proto::ErrorCategory errorCategory(commhub::HubErrorCode code) {
    switch (code) {
        case commhub::HubErrorCode::invalid_argument:
        case commhub::HubErrorCode::deadline_expired: return system_proto::INVALID_ARGUMENT;
        case commhub::HubErrorCode::service_unavailable:
        case commhub::HubErrorCode::route_not_found: return system_proto::SERVICE_UNAVAILABLE;
        case commhub::HubErrorCode::owner_unavailable: return system_proto::OWNER_UNAVAILABLE;
        case commhub::HubErrorCode::service_already_registered:
            return system_proto::SERVICE_ALREADY_REGISTERED;
        case commhub::HubErrorCode::resource_exhausted: return system_proto::RESOURCE_EXHAUSTED;
        case commhub::HubErrorCode::session_not_found:
        case commhub::HubErrorCode::none: return system_proto::PROTOCOL_ERROR;
    }
    return system_proto::PROTOCOL_ERROR;
}

Error transportError(const transport::Error& error) {
    return Error::connectionLost(error.message);
}

}  // namespace

struct CommHubServer::Peer {
    std::shared_ptr<transport::IConnection> connection;
    commhub::SessionId session_id;
};

CommHubServer::CommHubServer(std::shared_ptr<transport::IListener> listener,
                             transport::Endpoint endpoint,
                             std::string hub_instance_id,
                             std::string auth_token,
                             CommHubServerOptions options)
    : listener_(std::move(listener)),
      endpoint_(std::move(endpoint)),
      hub_instance_id_(hub_instance_id.empty() ? generateUuidV4()
                                                : std::move(hub_instance_id)),
      auth_token_(std::move(auth_token)),
      options_(options) {}

CommHubServer::~CommHubServer() { stop(); }

Result<void> CommHubServer::start() {
    if (!listener_ || endpoint_.empty() ||
        options_.heartbeat_timeout <= std::chrono::milliseconds::zero() ||
        options_.sweep_interval <= std::chrono::milliseconds::zero() ||
        options_.sweep_interval > options_.heartbeat_timeout) {
        return Error::invalidArgument(
            "CommHubServer requires listener, endpoint and valid heartbeat timing");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            return Error::invalidArgument("CommHubServer is already started");
        }
        started_ = true;
        maintenance_stop_ = false;
    }
    const auto listening = listener_->listen(
        endpoint_, [this](std::shared_ptr<transport::IConnection> connection) {
            accept(std::move(connection));
        });
    if (!listening) {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
        return transportError(listening.error());
    }
    try {
        maintenance_thread_ = std::thread([this]() noexcept {
            try {
                maintenanceLoop();
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                maintenance_stop_ = true;
            }
        });
    } catch (const std::exception& error) {
        try {
            listener_->close();
        } catch (...) {
            // Preserve the Result-based startup error path.
        }
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
        maintenance_stop_ = true;
        return Error::internal(std::string("failed to start CommHub maintenance: ") +
                               error.what());
    }
    return Result<void>::success();
}

void CommHubServer::stop() noexcept {
    std::thread maintenance;
    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ && !maintenance_thread_.joinable()) {
            return;
        }
        started_ = false;
        maintenance_stop_ = true;
        maintenance = std::move(maintenance_thread_);
    }
    maintenance_cv_.notify_all();
    if (maintenance.joinable()) {
        maintenance.join();
    }
    if (listener_) {
        try {
            listener_->close();
        } catch (...) {
            // stop() is noexcept; continue cleaning up registered peers.
        }
    }
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            peers.swap(peers_);
            sessions_.clear();
        }
        for (const auto& peer : peers) {
            if (!peer->session_id.empty()) {
                try {
                    core_.disconnect(peer->session_id);
                } catch (...) {
                    // Continue draining all sessions during best-effort shutdown.
                }
            }
        }
    }
    for (const auto& peer : peers) {
        try {
            peer->connection->close();
        } catch (...) {
            // One faulty transport peer must not abort server shutdown.
        }
    }
}

void CommHubServer::accept(std::shared_ptr<transport::IConnection> connection) {
    auto peer = std::make_shared<Peer>();
    peer->connection = std::move(connection);
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            peers_.push_back(peer);
            accepted = true;
        }
    }
    if (!accepted) {
        peer->connection->close();
        return;
    }
    std::weak_ptr<Peer> weak = peer;
    peer->connection->setReceiveHandler(
        [this, weak](transport::ByteBuffer frame) {
            if (auto current = weak.lock()) {
                receive(current, std::move(frame));
            }
        });
}

void CommHubServer::receive(const std::shared_ptr<Peer>& peer,
                            transport::ByteBuffer frame) {
    envelope::MessageEnvelope wire;
    if (!wire.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
        return;
    }
    if (wire.topic() == "system.service.register") {
        const auto status = registerPeer(peer, frame);
        if (!status) {
            sendRouteError(peer, frame,
                           {commhub::HubErrorCode::invalid_argument,
                            status.error().message});
        }
        return;
    }
    if (wire.topic() == "system.subscription.update") {
        const auto status = updateSubscription(peer, frame);
        if (!status) {
            sendRouteError(peer, frame,
                           {commhub::HubErrorCode::invalid_argument,
                            status.error().message});
        }
        return;
    }
    if (wire.topic() == "system.heartbeat" ||
        wire.message_type() == envelope::HEARTBEAT) {
        const auto status = handleHeartbeat(peer, frame);
        if (!status) {
            sendRouteError(peer, frame,
                           {commhub::HubErrorCode::invalid_argument,
                            status.error().message});
        }
        return;
    }
    if (peer->session_id.empty()) {
        sendRouteError(peer, frame,
                       {commhub::HubErrorCode::session_not_found,
                        "connection is not registered"});
        return;
    }
    auto route = decodeRouteMessage(frame);
    if (!route) {
        sendRouteError(peer, frame,
                       {commhub::HubErrorCode::invalid_argument,
                        route.error().message});
        return;
    }
    auto decision = core_.dispatch(peer->session_id, route.value(),
                                   std::chrono::steady_clock::now(), unixNowNs());
    if (!decision) {
        sendRouteError(peer, frame, decision.status.error);
        return;
    }
    drain(decision.targets);
}

Result<void> CommHubServer::registerPeer(const std::shared_ptr<Peer>& peer,
                                         const transport::ByteBuffer& frame) {
    envelope::MessageEnvelope wire;
    system_proto::ServiceRegistration registration;
    if (!wire.ParseFromArray(frame.data(), static_cast<int>(frame.size())) ||
        !registration.ParseFromString(wire.payload()) ||
        registration.service_name().empty() || registration.instance_id().empty()) {
        return Error::protocol("invalid ServiceRegistration");
    }
    const auto authorization = wire.headers().find("authorization");
    if (!auth_token_.empty() &&
        (authorization == wire.headers().end() ||
         authorization->second != auth_token_)) {
        return Error::make(ErrorCategory::PermissionDenied,
                           "invalid CommHub authentication token");
    }
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!peer->session_id.empty()) {
        return Error::protocol("connection is already registered");
    }

    commhub::Session session;
    session.id = registration.instance_id();
    session.node_id = registration.node_id();
    session.service_name = registration.service_name();
    session.instance_id = registration.instance_id();
    session.version = registration.version();
    const auto connected = core_.connect(session);
    if (!connected) {
        return Error::protocol(connected.error.message);
    }

    commhub::ServiceRegistration service;
    service.node_id = registration.node_id();
    service.service_name = registration.service_name();
    service.instance_id = registration.instance_id();
    service.version = registration.version();
    service.instance_mode = registration.instance_mode() == system_proto::MULTI_INSTANCE
                                ? commhub::InstanceMode::multi_instance
                                : commhub::InstanceMode::singleton_per_user;
    const auto registered = core_.registerService(session.id, std::move(service));
    if (!registered) {
        core_.disconnect(session.id);
        return Error::protocol(registered.error.message);
    }

    peer->session_id = session.id;
    bool installed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            sessions_[session.id] = peer;
            installed = true;
        }
    }
    if (!installed) {
        peer->session_id.clear();
        core_.disconnect(session.id);
        return Error::connectionLost("CommHub stopped during registration");
    }
    lifecycle_lock.unlock();

    system_proto::ServiceRegistrationAck ack;
    ack.set_hub_instance_id(hub_instance_id_);
    ack.set_negotiated_protocol_version(1);
    const auto reply = serializeReply(
        wire, "industrial_bus.system.v1.ServiceRegistrationAck",
        envelope::RESPONSE_OUTPUT, ack, "commhub", hub_instance_id_);
    const auto sent = peer->connection->send(transport::ByteView(reply));
    return sent ? Result<void>::success()
                : Result<void>::failure(transportError(sent.error()));
}

Result<void> CommHubServer::updateSubscription(const std::shared_ptr<Peer>& peer,
                                               const transport::ByteBuffer& frame) {
    if (peer->session_id.empty()) {
        return Error::protocol("subscription requires a registered session");
    }
    envelope::MessageEnvelope wire;
    system_proto::SubscriptionUpdate update;
    if (!wire.ParseFromArray(frame.data(), static_cast<int>(frame.size())) ||
        !update.ParseFromString(wire.payload()) || update.topic().empty()) {
        return Error::protocol("invalid SubscriptionUpdate");
    }
    if (update.subscribe()) {
        const auto status = core_.subscribe(peer->session_id, update.topic());
        return status ? Result<void>::success()
                      : Result<void>::failure(Error::protocol(status.error.message));
    }
    core_.unsubscribe(peer->session_id, update.topic());
    return Result<void>::success();
}

Result<void> CommHubServer::handleHeartbeat(
    const std::shared_ptr<Peer>& peer,
    const transport::ByteBuffer& frame) {
    if (peer->session_id.empty()) {
        return Error::protocol("heartbeat requires a registered session");
    }
    envelope::MessageEnvelope wire;
    system_proto::Heartbeat heartbeat;
    if (!wire.ParseFromArray(frame.data(), static_cast<int>(frame.size())) ||
        wire.topic() != "system.heartbeat" ||
        wire.message_type() != envelope::HEARTBEAT ||
        wire.payload_role() != envelope::SYSTEM_PAYLOAD ||
        wire.payload_type() != "industrial_bus.system.v1.Heartbeat" ||
        !heartbeat.ParseFromString(wire.payload()) || heartbeat.sequence() == 0) {
        return Error::protocol("invalid Heartbeat");
    }
    auto route = decodeRouteMessage(frame);
    if (!route) {
        return route.error();
    }
    const auto received_at_unix_ns = unixNowNs();
    const auto decision = core_.dispatch(peer->session_id, route.value(),
                                         std::chrono::steady_clock::now(),
                                         received_at_unix_ns);
    if (!decision) {
        return Error::protocol(decision.status.error.message);
    }

    system_proto::HeartbeatAck ack;
    ack.set_sent_at_unix_ns(heartbeat.sent_at_unix_ns());
    ack.set_received_at_unix_ns(received_at_unix_ns);
    ack.set_sequence(heartbeat.sequence());
    ack.set_hub_instance_id(hub_instance_id_);
    const auto reply = serializeReply(
        wire, "industrial_bus.system.v1.HeartbeatAck",
        envelope::SYSTEM_PAYLOAD, ack, "commhub", hub_instance_id_);
    const auto sent = peer->connection->send(transport::ByteView(reply));
    return sent ? Result<void>::success()
                : Result<void>::failure(transportError(sent.error()));
}

void CommHubServer::maintenanceLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!maintenance_stop_) {
        if (maintenance_cv_.wait_for(
                lock, options_.sweep_interval,
                [this] { return maintenance_stop_; })) {
            return;
        }
        lock.unlock();
        expireStaleSessions();
        lock.lock();
    }
}

void CommHubServer::expireStaleSessions() {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    const auto expired = core_.expireSessions(options_.heartbeat_timeout);
    if (expired.empty()) {
        return;
    }
    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& session_id : expired) {
            const auto found = sessions_.find(session_id);
            if (found == sessions_.end()) {
                continue;
            }
            const auto peer = found->second;
            sessions_.erase(found);
            peers_.erase(std::remove(peers_.begin(), peers_.end(), peer),
                         peers_.end());
            peers.push_back(peer);
        }
    }
    lifecycle_lock.unlock();
    for (const auto& peer : peers) {
        try {
            peer->connection->close();
        } catch (...) {
            // Keep the maintenance worker alive for the remaining sessions.
        }
    }
}

void CommHubServer::drain(const std::vector<commhub::SessionId>& targets) {
    for (const auto& target : targets) {
        std::shared_ptr<Peer> peer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = sessions_.find(target);
            if (found != sessions_.end()) {
                peer = found->second;
            }
        }
        if (!peer) {
            continue;
        }
        while (auto message = core_.nextOutbound(target)) {
            const auto sent = peer->connection->send(
                transport::ByteView(message->wire_frame));
            if (!sent) {
                break;
            }
        }
    }
}

void CommHubServer::sendRouteError(const std::shared_ptr<Peer>& peer,
                                   const transport::ByteBuffer& request,
                                   const commhub::HubError& error) {
    envelope::MessageEnvelope wire;
    if (!wire.ParseFromArray(request.data(), static_cast<int>(request.size()))) {
        return;
    }
    system_proto::Error detail;
    detail.set_category(errorCategory(error.code));
    detail.set_message(error.message);
    detail.set_retryable(error.code == commhub::HubErrorCode::service_unavailable ||
                         error.code == commhub::HubErrorCode::resource_exhausted);
    detail.set_operation_id(wire.operation_id());
    const auto reply = serializeReply(wire, "industrial_bus.system.v1.Error",
                                      envelope::ERROR_DETAIL, detail,
                                      "commhub", hub_instance_id_);
    peer->connection->send(transport::ByteView(reply));
}

}  // namespace industrial_bus::runtime
