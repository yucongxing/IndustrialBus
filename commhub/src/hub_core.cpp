#include "industrial_bus/commhub/hub_core.h"

namespace industrial_bus::commhub {

HubCore::HubCore(QoSConfig qos, std::chrono::milliseconds return_route_ttl)
    : return_routes_(return_route_ttl),
      qos_(qos),
      router_(sessions_, services_, return_routes_) {}

HubStatus HubCore::connect(Session session, SteadyTime now) {
    return sessions_.add(std::move(session), now);
}

bool HubCore::disconnect(const SessionId& session_id) {
    services_.unregisterSession(session_id);
    return_routes_.eraseForSession(session_id);
    qos_.remove(session_id);
    return sessions_.remove(session_id);
}

HubStatus HubCore::registerService(const SessionId& session_id,
                                   ServiceRegistration registration) {
    const auto session = sessions_.find(session_id);
    if (!session) {
        return HubStatus::failure(HubErrorCode::session_not_found,
                                  "cannot register a service for an unknown session");
    }
    if ((!session->service_name.empty() &&
         session->service_name != registration.service_name) ||
        (!session->instance_id.empty() &&
         session->instance_id != registration.instance_id) ||
        (!session->node_id.empty() && session->node_id != registration.node_id)) {
        return HubStatus::failure(HubErrorCode::invalid_argument,
                                  "service registration does not match its session identity");
    }
    return services_.registerService(session_id, std::move(registration));
}

HubStatus HubCore::subscribe(const SessionId& session_id, std::string topic) {
    return sessions_.subscribe(session_id, std::move(topic));
}

bool HubCore::unsubscribe(const SessionId& session_id, const std::string& topic) {
    return sessions_.unsubscribe(session_id, topic);
}

std::vector<SessionId> HubCore::expireSessions(std::chrono::milliseconds timeout,
                                               SteadyTime now) {
    auto expired = sessions_.stale(timeout, now);
    for (const auto& session_id : expired) {
        disconnect(session_id);
    }
    return expired;
}

RouteDecision HubCore::dispatch(const SessionId& source_session,
                                const RouteMessage& message,
                                SteadyTime now,
                                std::int64_t now_unix_ns) {
    auto decision = router_.route(source_session, message, now, now_unix_ns);
    if (!decision) {
        return decision;
    }
    for (const auto& target : decision.targets) {
        auto enqueue = qos_.enqueue(target, message);
        if (enqueue.disposition == EnqueueDisposition::disconnect_slow_consumer) {
            disconnect(target);
            decision.status = enqueue.status;
            return decision;
        }
        if (enqueue.disposition == EnqueueDisposition::rejected) {
            if (decision.return_route_stored) {
                return_routes_.erase(message.message_id);
                decision.return_route_stored = false;
            }
            decision.status = enqueue.status;
            return decision;
        }
    }
    return decision;
}

std::optional<RouteMessage> HubCore::nextOutbound(const SessionId& target) {
    return qos_.pop(target);
}

}  // namespace industrial_bus::commhub
