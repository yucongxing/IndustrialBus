#include "industrial_bus/commhub/hub_core.h"

#include <gtest/gtest.h>

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

ServiceRegistration registration(
    std::string service, std::string instance,
    InstanceMode mode = InstanceMode::singleton_per_user) {
    ServiceRegistration value;
    value.node_id = "local";
    value.service_name = std::move(service);
    value.instance_id = std::move(instance);
    value.instance_mode = mode;
    return value;
}

TEST(CommHubRouterTest, RoutesExactlyAcrossMultipleServiceInstances) {
    const auto now = std::chrono::steady_clock::now();
    constexpr std::int64_t unix_now = 1'000'000'000;
    HubCore hub;

    ASSERT_TRUE(hub.connect(session("caller", "caller", "caller-1"), now));
    ASSERT_TRUE(hub.connect(session("cmp-a", "cmp-suite", "channel-a"), now));
    ASSERT_TRUE(hub.connect(session("cmp-b", "cmp-suite", "channel-b"), now));
    ASSERT_TRUE(hub.registerService(
        "cmp-a", registration("cmp-suite", "channel-a",
                              InstanceMode::multi_instance)));
    ASSERT_TRUE(hub.registerService(
        "cmp-b", registration("cmp-suite", "channel-b",
                              InstanceMode::multi_instance)));

    RouteMessage command;
    command.message_id = "command-b";
    command.source_service = "caller";
    command.source_instance = "caller-1";
    command.destination_service = "cmp-suite";
    command.destination_instance = "channel-b";
    command.topic = "cmp.channel.channel-b.simulation.run";
    command.kind = MessageKind::command;
    command.deadline_unix_ns = unix_now + 1'000'000'000;
    const auto routed = hub.dispatch("caller", command, now, unix_now);
    ASSERT_TRUE(routed);
    ASSERT_EQ(routed.targets.size(), 1U);
    EXPECT_EQ(routed.targets.front(), "cmp-b");

    EXPECT_FALSE(hub.services().resolve("cmp-suite", {},
                                        SelectionPolicy::prefer_local,
                                        "local"));
    EXPECT_EQ(hub.services().allInstances().size(), 2U);

    ASSERT_TRUE(hub.connect(session("singleton", "cmp-suite", "channel-c"),
                            now));
    const auto conflict = hub.registerService(
        "singleton", registration("cmp-suite", "channel-c"));
    EXPECT_FALSE(conflict);
    EXPECT_EQ(conflict.error.code, HubErrorCode::service_already_registered);
}

}  // namespace

TEST(CommHubRouterTest, RoutesRequestsRepliesEventsAndRejectsInvalidRoutes) {
    const auto now = std::chrono::steady_clock::now();
    constexpr std::int64_t unix_now = 1'000'000'000;
    HubCore hub({{{2, 2, 2, 2}}}, std::chrono::seconds(10));

    ASSERT_TRUE(hub.connect(session("caller-session", "caller", "caller-1"), now));
    ASSERT_TRUE(hub.connect(session("worker-session", "worker", "worker-1"), now));
    ASSERT_TRUE(hub.connect(session("observer-session", "observer", "observer-1"), now));
    ASSERT_TRUE(hub.registerService("worker-session", registration("worker", "worker-1")));
    ASSERT_TRUE(hub.subscribe("observer-session", "worker.changed"));

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
    ASSERT_TRUE(routed);
    EXPECT_TRUE(routed.return_route_stored);
    ASSERT_EQ(routed.targets.size(), 1U);
    EXPECT_EQ(routed.targets.front(), "worker-session");
    EXPECT_EQ(hub.returnRoutes().size(), 1U);
    const auto to_worker = hub.nextOutbound("worker-session");
    ASSERT_TRUE(to_worker.has_value());
    EXPECT_EQ(to_worker->message_id, "request-1");

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
    ASSERT_TRUE(routed);
    ASSERT_FALSE(routed.targets.empty());
    EXPECT_EQ(routed.targets.front(), "caller-session");
    EXPECT_EQ(hub.returnRoutes().size(), 0U);
    EXPECT_TRUE(hub.nextOutbound("caller-session").has_value());

    RouteMessage event;
    event.message_id = "event-1";
    event.source_service = "worker";
    event.source_instance = "worker-1";
    event.topic = "worker.changed";
    event.kind = MessageKind::event;
    routed = hub.dispatch("worker-session", event, now, unix_now);
    ASSERT_TRUE(routed);
    ASSERT_EQ(routed.targets.size(), 1U);
    EXPECT_EQ(routed.targets.front(), "observer-session");

    ASSERT_TRUE(hub.connect(session("duplicate-session", "worker", "worker-2"), now));
    const auto duplicate = hub.registerService(
        "duplicate-session", registration("worker", "worker-2"));
    EXPECT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error.code, HubErrorCode::service_already_registered);

    RouteMessage expired = request;
    expired.message_id = "expired-request";
    expired.deadline_unix_ns = unix_now;
    routed = hub.dispatch("caller-session", expired, now, unix_now);
    EXPECT_FALSE(routed);
    EXPECT_EQ(routed.status.error.code, HubErrorCode::deadline_expired);
}
