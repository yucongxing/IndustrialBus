#pragma once

#include "industrial_bus/commhub/return_route_table.h"
#include "industrial_bus/commhub/service_registry.h"
#include "industrial_bus/commhub/session_registry.h"

namespace industrial_bus::commhub {

class MessageRouter {
public:
    MessageRouter(SessionRegistry& sessions,
                  ServiceRegistry& services,
                  ReturnRouteTable& return_routes);

    RouteDecision route(const SessionId& source_session,
                        const RouteMessage& message,
                        SteadyTime now,
                        std::int64_t now_unix_ns);

private:
    SessionRegistry& sessions_;
    ServiceRegistry& services_;
    ReturnRouteTable& return_routes_;
};

}  // namespace industrial_bus::commhub
