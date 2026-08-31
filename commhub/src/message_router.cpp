#include "industrial_bus/commhub/message_router.h"

namespace industrial_bus::commhub {
namespace {

RouteDecision failure(HubErrorCode code, std::string message) {
    return {HubStatus::failure(code, std::move(message)), {}, false};
}

}  // namespace

MessageRouter::MessageRouter(SessionRegistry& sessions,
                             ServiceRegistry& services,
                             ReturnRouteTable& return_routes)
    : sessions_(sessions), services_(services), return_routes_(return_routes) {}

RouteDecision MessageRouter::route(const SessionId& source_session,
                                   const RouteMessage& message,
                                   SteadyTime now,
                                   std::int64_t now_unix_ns) {
    const auto source = sessions_.find(source_session);
    if (!source) {
        return failure(HubErrorCode::session_not_found, "source session is not registered");
    }
    if (message.message_id.empty() || message.topic.empty()) {
        return failure(HubErrorCode::invalid_argument, "message id and topic are required");
    }
    if ((!message.source_service.empty() && message.source_service != source->service_name) ||
        (!message.source_instance.empty() && message.source_instance != source->instance_id)) {
        return failure(HubErrorCode::invalid_argument,
                       "message source does not match authenticated session");
    }

    if (message.kind == MessageKind::reply) {
        if (message.causation_id.empty()) {
            return failure(HubErrorCode::invalid_argument,
                           "reply causation id must identify its request");
        }
        const auto return_route = return_routes_.take(message.causation_id, source_session, now);
        if (!return_route) {
            return failure(HubErrorCode::route_not_found,
                           "reply has no live return route");
        }
        return {HubStatus::success(), {return_route->source_session}, false};
    }

    if (message.kind == MessageKind::event || message.kind == MessageKind::progress ||
        message.kind == MessageKind::log) {
        return {HubStatus::success(), sessions_.subscribers(message.topic, source_session), false};
    }

    if (message.kind == MessageKind::heartbeat) {
        sessions_.touch(source_session, now);
        return {HubStatus::success(), {}, false};
    }

    if (message.destination_service.empty()) {
        return failure(HubErrorCode::invalid_argument,
                       "directed message requires a destination service");
    }
    if (message.kind == MessageKind::request || message.kind == MessageKind::command) {
        if (message.deadline_unix_ns == 0) {
            return failure(HubErrorCode::invalid_argument,
                           "request/command requires an absolute deadline");
        }
    }
    if ((message.kind == MessageKind::request || message.kind == MessageKind::command) &&
        message.deadline_unix_ns <= now_unix_ns) {
        return failure(HubErrorCode::deadline_expired,
                       "message arrived after its absolute deadline");
    }

    const auto destination = services_.resolve(message.destination_service,
                                               message.destination_instance,
                                               message.selection_policy,
                                               source->node_id);
    if (!destination) {
        const bool owner_route = !message.operation_id.empty() &&
                                 !message.destination_instance.empty();
        return failure(owner_route ? HubErrorCode::owner_unavailable
                                   : HubErrorCode::service_unavailable,
                       owner_route ? "operation owner instance is unavailable"
                                   : "destination service is unavailable or ambiguous");
    }

    bool stored = false;
    if (message.kind == MessageKind::request) {
        const auto status = return_routes_.put(message.message_id,
                                               source_session,
                                               destination->session_id,
                                               message.deadline_unix_ns,
                                               now,
                                               now_unix_ns);
        if (!status) {
            return {status, {}, false};
        }
        stored = true;
    }
    return {HubStatus::success(), {destination->session_id}, stored};
}

}  // namespace industrial_bus::commhub
