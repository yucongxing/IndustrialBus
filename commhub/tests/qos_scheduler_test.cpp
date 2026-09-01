#include "industrial_bus/commhub/qos_scheduler.h"

#include <gtest/gtest.h>

using namespace industrial_bus::commhub;

TEST(CommHubQosSchedulerTest, CoalescesAndAppliesOverflowPolicies) {
    QoSConfig config;
    config.capacity = {{1, 1, 1, 1}};
    QoSScheduler qos(config);

    RouteMessage progress;
    progress.message_id = "progress-1";
    progress.operation_id = "operation-1";
    progress.kind = MessageKind::progress;
    progress.priority = 2;
    ASSERT_TRUE(qos.enqueue("target", progress));
    progress.message_id = "progress-2";
    const auto coalesced = qos.enqueue("target", progress);
    EXPECT_EQ(coalesced.disposition, EnqueueDisposition::coalesced);
    const auto latest = qos.pop("target");
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->message_id, "progress-2");

    RouteMessage command;
    command.kind = MessageKind::command;
    command.priority = 1;
    ASSERT_TRUE(qos.enqueue("target", command));
    const auto rejected = qos.enqueue("target", command);
    EXPECT_EQ(rejected.disposition, EnqueueDisposition::rejected);
    EXPECT_EQ(rejected.status.error.code, HubErrorCode::resource_exhausted);

    RouteMessage reply;
    reply.kind = MessageKind::reply;
    reply.priority = 0;
    ASSERT_TRUE(qos.enqueue("slow", reply));
    const auto overflow = qos.enqueue("slow", reply);
    EXPECT_EQ(overflow.disposition,
              EnqueueDisposition::disconnect_slow_consumer);
    EXPECT_EQ(qos.stats("slow").critical_overflow, 1U);
}
