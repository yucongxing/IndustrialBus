#pragma once

#include "industrial_bus/qos.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace industrial_bus::commhub {

using SessionId = std::string;
using ByteBuffer = std::vector<std::uint8_t>;
using SteadyTime = std::chrono::steady_clock::time_point;

enum class HubErrorCode {
    none = 0,
    invalid_argument,
    session_not_found,
    service_unavailable,
    owner_unavailable,
    service_already_registered,
    route_not_found,
    deadline_expired,
    resource_exhausted,
};

struct HubError {
    HubErrorCode code{HubErrorCode::none};
    std::string message;
};

struct HubStatus {
    HubError error;

    explicit operator bool() const noexcept { return error.code == HubErrorCode::none; }
    static HubStatus success() { return {}; }
    static HubStatus failure(HubErrorCode code, std::string message) {
        return {{code, std::move(message)}};
    }
};

enum class MessageKind {
    command,
    event,
    request,
    reply,
    progress,
    error,
    log,
    heartbeat,
    system,
};

enum class PayloadRole {
    unspecified,
    command_input,
    command_ack,
    request_input,
    response_output,
    event_fact,
    error_detail,
    system_payload,
};

enum class InstanceMode {
    singleton_per_user,
    multi_instance,
};

enum class SelectionPolicy {
    unspecified,
    exact,
    prefer_local,
};

struct RouteMessage {
    std::string message_id;
    std::string correlation_id;
    std::string causation_id;
    std::string operation_id;
    std::string source_node;
    std::string source_service;
    std::string source_instance;
    std::string destination_node;
    std::string destination_service;
    std::string destination_instance;
    std::string topic;
    std::string payload_type;
    std::string idempotency_key;
    MessageKind kind{MessageKind::event};
    ::industrial_bus::MessageClass message_class{
        ::industrial_bus::MessageClass::Event};
    PayloadRole payload_role{PayloadRole::unspecified};
    SelectionPolicy selection_policy{SelectionPolicy::unspecified};
    std::uint32_t priority{2};
    std::int64_t deadline_unix_ns{0};
    ByteBuffer wire_frame;
};

struct Session {
    SessionId id;
    std::string node_id;
    std::string service_name;
    std::string instance_id;
    std::string version;
    std::vector<std::string> capabilities;
    std::unordered_set<std::string> subscriptions;
    std::uint32_t protocol_min{1};
    std::uint32_t protocol_max{1};
    SteadyTime registered_at{};
    SteadyTime last_heartbeat{};
};

struct ServiceRegistration {
    std::string node_id;
    std::string service_name;
    std::string instance_id;
    std::string version;
    std::vector<std::string> capabilities;
    InstanceMode instance_mode{InstanceMode::singleton_per_user};
};

struct ServiceInstance {
    SessionId session_id;
    ServiceRegistration registration;
};

struct ReturnRoute {
    std::string request_message_id;
    SessionId source_session;
    SessionId destination_session;
    std::int64_t deadline_unix_ns{0};
    SteadyTime expires_at{};
};

struct RouteDecision {
    HubStatus status;
    std::vector<SessionId> targets;
    bool return_route_stored{false};

    explicit operator bool() const noexcept { return static_cast<bool>(status); }
};

}  // namespace industrial_bus::commhub
