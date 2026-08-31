#include "industrial_bus/runtime/commhub_server.h"

#include "industrial_bus/ids.h"
#include "industrial_bus/runtime/envelope_codec.h"

#include "envelope.pb.h"
#include "system.pb.h"

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
                             std::string hub_instance_id)
    : listener_(std::move(listener)),
      endpoint_(std::move(endpoint)),
      hub_instance_id_(hub_instance_id.empty() ? generateUuidV4()
                                                : std::move(hub_instance_id)) {}

CommHubServer::~CommHubServer() { stop(); }

Result<void> CommHubServer::start() {
    if (!listener_ || endpoint_.empty()) {
        return Error::invalidArgument("CommHubServer requires listener and endpoint");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            return Error::invalidArgument("CommHubServer is already started");
        }
        started_ = true;
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
    return Result<void>::success();
}

void CommHubServer::stop() noexcept {
    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        started_ = false;
        peers.swap(peers_);
        sessions_.clear();
    }
    if (listener_) {
        listener_->close();
    }
    for (const auto& peer : peers) {
        if (!peer->session_id.empty()) {
            core_.disconnect(peer->session_id);
        }
        peer->connection->close();
    }
}

void CommHubServer::accept(std::shared_ptr<transport::IConnection> connection) {
    auto peer = std::make_shared<Peer>();
    peer->connection = std::move(connection);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            peer->connection->close();
            return;
        }
        peers_.push_back(peer);
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[session.id] = peer;
    }

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
