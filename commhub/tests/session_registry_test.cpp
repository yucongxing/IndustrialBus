#include "industrial_bus/commhub/hub_core.h"

#include <gtest/gtest.h>

#include <chrono>
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

TEST(CommHubSessionRegistryTest, ExpiresStaleSessionsAndPreservesLiveSessions) {
    using namespace std::chrono_literals;

    HubCore hub;
    const auto started = SteadyTime{} + 10s;
    const auto stale_session = session("stale", "stale-service");
    const auto live_session = session("live", "live-service");
    ASSERT_TRUE(hub.connect(stale_session, started));
    ASSERT_TRUE(hub.connect(live_session, started));
    ASSERT_TRUE(hub.registerService(stale_session.id,
                                    registration(stale_session)));
    ASSERT_TRUE(hub.registerService(live_session.id,
                                    registration(live_session)));
    ASSERT_TRUE(hub.sessions().touch(live_session.id, started + 4s));

    const auto expired = hub.expireSessions(5s, started + 6s);
    ASSERT_EQ(expired.size(), 1U);
    EXPECT_EQ(expired.front(), stale_session.id);
    EXPECT_FALSE(hub.sessions().find(stale_session.id).has_value());
    EXPECT_FALSE(hub.services()
                     .exact(stale_session.service_name,
                            stale_session.instance_id)
                     .has_value());
    EXPECT_TRUE(hub.sessions().find(live_session.id).has_value());
    EXPECT_TRUE(hub.services()
                    .exact(live_session.service_name,
                           live_session.instance_id)
                    .has_value());
    EXPECT_TRUE(hub.expireSessions(0ms, started + 60s).empty());
}
