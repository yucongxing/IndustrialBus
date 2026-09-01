#include "industrial_bus/commhub/hub_core.h"

#include <chrono>
#include <iostream>
#include <string>

namespace {

using namespace industrial_bus::commhub;

Session session(std::string id, std::string service) {
    Session value;
    value.id = std::move(id);
    value.node_id = "local";
    value.service_name = std::move(service);
    value.instance_id = value.id + "-instance";
    return value;
}

ServiceRegistration registration(const Session& session) {
    ServiceRegistration value;
    value.node_id = session.node_id;
    value.service_name = session.service_name;
    value.instance_id = session.instance_id;
    return value;
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    HubCore hub;
    const auto started = SteadyTime{} + 10s;
    const auto stale_session = session("stale", "stale-service");
    const auto live_session = session("live", "live-service");
    if (!hub.connect(stale_session, started) ||
        !hub.connect(live_session, started) ||
        !hub.registerService(stale_session.id, registration(stale_session)) ||
        !hub.registerService(live_session.id, registration(live_session))) {
        std::cerr << "failed to prepare sessions\n";
        return 1;
    }
    if (!hub.sessions().touch(live_session.id, started + 4s)) {
        std::cerr << "failed to refresh live session\n";
        return 1;
    }

    const auto expired = hub.expireSessions(5s, started + 6s);
    if (expired.size() != 1 || expired.front() != stale_session.id) {
        std::cerr << "unexpected expired session set\n";
        return 1;
    }
    if (hub.sessions().find(stale_session.id) ||
        hub.services().exact(stale_session.service_name,
                             stale_session.instance_id)) {
        std::cerr << "stale service state was not removed\n";
        return 1;
    }
    if (!hub.sessions().find(live_session.id) ||
        !hub.services().exact(live_session.service_name,
                              live_session.instance_id)) {
        std::cerr << "live service was removed\n";
        return 1;
    }
    if (!hub.expireSessions(0ms, started + 60s).empty()) {
        std::cerr << "non-positive timeout expired sessions\n";
        return 1;
    }
    return 0;
}
