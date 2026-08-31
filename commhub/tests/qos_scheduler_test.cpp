#include "industrial_bus/commhub/qos_scheduler.h"

#include <cassert>

using namespace industrial_bus::commhub;

int main() {
    QoSConfig config;
    config.capacity = {{1, 1, 1, 1}};
    QoSScheduler qos(config);

    RouteMessage progress;
    progress.message_id = "progress-1";
    progress.operation_id = "operation-1";
    progress.kind = MessageKind::progress;
    progress.priority = 2;
    assert(qos.enqueue("target", progress));
    progress.message_id = "progress-2";
    const auto coalesced = qos.enqueue("target", progress);
    assert(coalesced.disposition == EnqueueDisposition::coalesced);
    const auto latest = qos.pop("target");
    assert(latest && latest->message_id == "progress-2");

    RouteMessage command;
    command.kind = MessageKind::command;
    command.priority = 1;
    assert(qos.enqueue("target", command));
    const auto rejected = qos.enqueue("target", command);
    assert(rejected.disposition == EnqueueDisposition::rejected);
    assert(rejected.status.error.code == HubErrorCode::resource_exhausted);

    RouteMessage reply;
    reply.kind = MessageKind::reply;
    reply.priority = 0;
    assert(qos.enqueue("slow", reply));
    const auto overflow = qos.enqueue("slow", reply);
    assert(overflow.disposition == EnqueueDisposition::disconnect_slow_consumer);
    assert(qos.stats("slow").critical_overflow == 1);
}
