#include "industrial_bus/commhub/hub_core.h"

#include <cassert>
#include <chrono>

using namespace industrial_bus::commhub;

namespace {

Session session(std::string id, std::string service, std::string instance) {
    Session value;
    value.id = std::move(id);
    value.node_id = "local";
    value.service_name = std::move(service);
    value.instance_id = std::move(instance);
    return value;
}

ServiceRegistration registration(std::string service, std::string instance) {
    ServiceRegistration value;
    value.node_id = "local";
    value.service_name = std::move(service);
    value.instance_id = std::move(instance);
    value.instance_mode = InstanceMode::singleton_per_user;
    return value;
}

}  // namespace

int main() {
    const auto now = std::chrono::steady_clock::now();
    constexpr std::int64_t unix_now = 1'000'000'000;
    HubCore hub({{{2, 2, 2, 2}}}, std::chrono::seconds(10));

    assert(hub.connect(session("caller-session", "caller", "caller-1"), now));
    assert(hub.connect(session("worker-session", "worker", "worker-1"), now));
    assert(hub.connect(session("observer-session", "observer", "observer-1"), now));
    assert(hub.registerService("worker-session", registration("worker", "worker-1")));
    assert(hub.subscribe("observer-session", "worker.changed"));

    RouteMessage request;
    request.message_id = "request-1";
    request.correlation_id = "flow-1";
    request.source_service = "caller";
    request.source_instance = "caller-1";
    request.destination_service = "worker";
    request.topic = "worker.get";
    request.kind = MessageKind::request;
    request.deadline_unix_ns = unix_now + 5'000'000'000;

    auto routed = hub.dispatch("caller-session", request, now, unix_now);
    assert(routed);
    assert(routed.return_route_stored);
    assert(routed.targets.size() == 1 && routed.targets.front() == "worker-session");
    assert(hub.returnRoutes().size() == 1);
    const auto to_worker = hub.nextOutbound("worker-session");
    assert(to_worker && to_worker->message_id == "request-1");

    RouteMessage reply;
    reply.message_id = "reply-1";
    reply.correlation_id = "flow-1";
    reply.causation_id = "request-1";
    reply.source_service = "worker";
    reply.source_instance = "worker-1";
    reply.topic = "worker.get";
    reply.kind = MessageKind::reply;
    reply.priority = 0;
    routed = hub.dispatch("worker-session", reply, now, unix_now);
    assert(routed && routed.targets.front() == "caller-session");
    assert(hub.returnRoutes().size() == 0);
    assert(hub.nextOutbound("caller-session"));

    RouteMessage event;
    event.message_id = "event-1";
    event.source_service = "worker";
    event.source_instance = "worker-1";
    event.topic = "worker.changed";
    event.kind = MessageKind::event;
    routed = hub.dispatch("worker-session", event, now, unix_now);
    assert(routed && routed.targets.size() == 1);
    assert(routed.targets.front() == "observer-session");

    assert(hub.connect(session("duplicate-session", "worker", "worker-2"), now));
    const auto duplicate = hub.registerService(
        "duplicate-session", registration("worker", "worker-2"));
    assert(!duplicate);
    assert(duplicate.error.code == HubErrorCode::service_already_registered);

    RouteMessage expired = request;
    expired.message_id = "expired-request";
    expired.deadline_unix_ns = unix_now;
    routed = hub.dispatch("caller-session", expired, now, unix_now);
    assert(!routed);
    assert(routed.status.error.code == HubErrorCode::deadline_expired);
}
