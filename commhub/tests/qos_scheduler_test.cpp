#include "industrial_bus/commhub/qos_scheduler.h"
#include "industrial_bus/qos.h"

#include <gtest/gtest.h>

using namespace industrial_bus::commhub;

namespace {

RouteMessage message(std::string id,
                     MessageKind kind,
                     industrial_bus::MessageClass message_class,
                     std::uint32_t priority,
                     std::string operation_id = {}) {
    RouteMessage value;
    value.message_id = std::move(id);
    value.kind = kind;
    value.message_class = message_class;
    value.priority = priority;
    value.operation_id = std::move(operation_id);
    return value;
}

}  // namespace

TEST(CommHubQosSchedulerTest, CoalescesAndAppliesOverflowPolicies) {
    QoSConfig config;
    config.capacity = 1;
    QoSScheduler qos(config);

    RouteMessage progress;
    progress.message_id = "progress-1";
    progress.operation_id = "operation-1";
    progress.kind = MessageKind::progress;
    progress.message_class = industrial_bus::MessageClass::Progress;
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
    command.message_class = industrial_bus::MessageClass::Command;
    command.priority = 1;
    ASSERT_TRUE(qos.enqueue("target", command));
    const auto rejected = qos.enqueue("target", command);
    EXPECT_EQ(rejected.disposition, EnqueueDisposition::rejected);
    EXPECT_EQ(rejected.status.error.code, HubErrorCode::resource_exhausted);

    RouteMessage reply;
    reply.kind = MessageKind::reply;
    reply.message_class = industrial_bus::MessageClass::Reply;
    reply.priority = 0;
    ASSERT_TRUE(qos.enqueue("slow", reply));
    const auto overflow = qos.enqueue("slow", reply);
    EXPECT_EQ(overflow.disposition,
              EnqueueDisposition::disconnect_slow_consumer);
    EXPECT_EQ(qos.stats("slow").critical_overflow, 1U);
}

TEST(CommHubQosSchedulerTest, OrdersByPriorityAndFifoWithinPriority) {
    QoSConfig config;
    config.capacity = 8;
    QoSScheduler qos(config);

    ASSERT_TRUE(qos.enqueue("target", message(
        "p3-a", MessageKind::log, industrial_bus::MessageClass::InfoLog, 3)));
    ASSERT_TRUE(qos.enqueue("target", message(
        "p2-a", MessageKind::event, industrial_bus::MessageClass::Event, 2)));
    ASSERT_TRUE(qos.enqueue("target", message(
        "p1-a", MessageKind::request, industrial_bus::MessageClass::Request, 1)));
    ASSERT_TRUE(qos.enqueue("target", message(
        "p0-a", MessageKind::reply, industrial_bus::MessageClass::Reply, 0)));
    ASSERT_TRUE(qos.enqueue("target", message(
        "p2-b", MessageKind::event, industrial_bus::MessageClass::Event, 2)));

    EXPECT_EQ(qos.pop("target")->message_id, "p0-a");
    EXPECT_EQ(qos.pop("target")->message_id, "p1-a");
    EXPECT_EQ(qos.pop("target")->message_id, "p2-a");
    EXPECT_EQ(qos.pop("target")->message_id, "p2-b");
    EXPECT_EQ(qos.pop("target")->message_id, "p3-a");
    EXPECT_EQ(qos.stats("target").dequeued, 5U);
}

TEST(CommHubQosSchedulerTest, CoalescesOnlyMatchingProgressOperation) {
    QoSConfig config;
    config.capacity = 4;
    QoSScheduler qos(config);

    ASSERT_TRUE(qos.enqueue("target", message(
        "a-10", MessageKind::progress, industrial_bus::MessageClass::Progress,
        2, "operation-a")));
    EXPECT_EQ(qos.enqueue("target", message(
                  "a-20", MessageKind::progress,
                  industrial_bus::MessageClass::Progress, 2, "operation-a"))
                  .disposition,
              EnqueueDisposition::coalesced);
    ASSERT_TRUE(qos.enqueue("target", message(
        "b-10", MessageKind::progress, industrial_bus::MessageClass::Progress,
        2, "operation-b")));
    ASSERT_TRUE(qos.enqueue("target", message(
        "terminal-a", MessageKind::event,
        industrial_bus::MessageClass::FinalState, 0, "operation-a")));

    EXPECT_EQ(qos.pop("target")->message_id, "terminal-a");
    EXPECT_EQ(qos.pop("target")->message_id, "a-20");
    EXPECT_EQ(qos.pop("target")->message_id, "b-10");
    EXPECT_EQ(qos.stats("target").progress_coalesced, 1U);
}

TEST(CommHubQosSchedulerTest, EvictsDisposableTrafficForMoreValuableInput) {
    QoSConfig config;
    config.capacity = 2;
    QoSScheduler qos(config);

    ASSERT_TRUE(qos.enqueue("target", message(
        "debug", MessageKind::log, industrial_bus::MessageClass::DebugLog, 3)));
    ASSERT_TRUE(qos.enqueue("target", message(
        "info", MessageKind::log, industrial_bus::MessageClass::InfoLog, 3)));
    ASSERT_TRUE(qos.enqueue("target", message(
        "command", MessageKind::command,
        industrial_bus::MessageClass::Command, 1)));

    EXPECT_EQ(qos.pop("target")->message_id, "command");
    EXPECT_EQ(qos.pop("target")->message_id, "info");
    EXPECT_EQ(qos.stats("target").dropped, 1U);
}

TEST(CommHubQosSchedulerTest, DistinguishesDropRejectAndCriticalOverflow) {
    QoSConfig config;
    config.capacity = 1;
    QoSScheduler qos(config);

    ASSERT_TRUE(qos.enqueue("drop", message(
        "event", MessageKind::event, industrial_bus::MessageClass::Event, 2)));
    const auto dropped = qos.enqueue("drop", message(
        "progress", MessageKind::progress,
        industrial_bus::MessageClass::Progress, 2, "operation"));
    EXPECT_EQ(dropped.disposition, EnqueueDisposition::dropped);
    EXPECT_EQ(qos.stats("drop").dropped, 1U);

    ASSERT_TRUE(qos.enqueue("reject", message(
        "command", MessageKind::command,
        industrial_bus::MessageClass::Command, 1)));
    const auto rejected = qos.enqueue("reject", message(
        "request", MessageKind::request,
        industrial_bus::MessageClass::Request, 1));
    EXPECT_EQ(rejected.disposition, EnqueueDisposition::rejected);
    EXPECT_EQ(rejected.status.error.code, HubErrorCode::resource_exhausted);
    EXPECT_EQ(qos.stats("reject").resource_exhausted, 1U);
    EXPECT_EQ(qos.stats("reject").rejected, 1U);

    ASSERT_TRUE(qos.enqueue("critical", message(
        "reply", MessageKind::reply, industrial_bus::MessageClass::Reply, 0)));
    const auto overflow = qos.enqueue("critical", message(
        "accepted", MessageKind::reply,
        industrial_bus::MessageClass::Accepted, 0));
    EXPECT_EQ(overflow.disposition,
              EnqueueDisposition::disconnect_slow_consumer);
    EXPECT_EQ(qos.stats("critical").critical_overflow, 1U);
    EXPECT_EQ(qos.stats("critical").rejected, 1U);
}

TEST(CommHubQosSchedulerTest, ReportsZeroCapacityOutcomes) {
    QoSConfig config;
    config.capacity = 0;
    QoSScheduler qos(config);

    EXPECT_EQ(qos.enqueue("drop", message(
                  "event", MessageKind::event,
                  industrial_bus::MessageClass::Event, 2))
                  .disposition,
              EnqueueDisposition::dropped);
    EXPECT_EQ(qos.enqueue("reject", message(
                  "command", MessageKind::command,
                  industrial_bus::MessageClass::Command, 1))
                  .disposition,
              EnqueueDisposition::rejected);
    EXPECT_EQ(qos.enqueue("critical", message(
                  "reply", MessageKind::reply,
                  industrial_bus::MessageClass::Reply, 0))
                  .disposition,
              EnqueueDisposition::disconnect_slow_consumer);
}
