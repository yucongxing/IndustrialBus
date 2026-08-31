#pragma once

#include "industrial_bus/commhub/message_router.h"
#include "industrial_bus/commhub/qos_scheduler.h"

#include <chrono>
#include <optional>

namespace industrial_bus::commhub {

// Transport- and codec-independent CommHub state machine. The I/O adapter
// authenticates a connection, decodes its protobuf Envelope to RouteMessage,
// then calls dispatch. Outbound frames are drained per session.
class HubCore {
public:
    explicit HubCore(QoSConfig qos = {},
                     std::chrono::milliseconds return_route_ttl =
                         std::chrono::seconds(30));

    HubStatus connect(Session session,
                      SteadyTime now = std::chrono::steady_clock::now());
    bool disconnect(const SessionId& session_id);
    HubStatus registerService(const SessionId& session_id,
                              ServiceRegistration registration);
    HubStatus subscribe(const SessionId& session_id, std::string topic);
    bool unsubscribe(const SessionId& session_id, const std::string& topic);

    RouteDecision dispatch(const SessionId& source_session,
                           const RouteMessage& message,
                           SteadyTime now,
                           std::int64_t now_unix_ns);
    std::optional<RouteMessage> nextOutbound(const SessionId& target);

    SessionRegistry& sessions() noexcept { return sessions_; }
    const SessionRegistry& sessions() const noexcept { return sessions_; }
    ServiceRegistry& services() noexcept { return services_; }
    const ServiceRegistry& services() const noexcept { return services_; }
    ReturnRouteTable& returnRoutes() noexcept { return return_routes_; }
    QoSScheduler& qos() noexcept { return qos_; }

private:
    SessionRegistry sessions_;
    ServiceRegistry services_;
    ReturnRouteTable return_routes_;
    QoSScheduler qos_;
    MessageRouter router_;
};

}  // namespace industrial_bus::commhub
