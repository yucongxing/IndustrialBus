#include "industrial_bus/commhub/qos_scheduler.h"

namespace industrial_bus::commhub {

QoSScheduler::QoSScheduler(QoSConfig config) : config_(config) {}

bool QoSScheduler::validSemantics(const RouteMessage& message) {
    if (message.priority > static_cast<std::uint32_t>(QosPriority::BestEffort) ||
        message.priority !=
            static_cast<std::uint32_t>(defaultPriority(message.message_class))) {
        return false;
    }
    const auto expected = messageTypeFor(message.message_class);
    switch (message.kind) {
        case MessageKind::command: return expected == MessageType::Command;
        case MessageKind::event: return expected == MessageType::Event;
        case MessageKind::request: return expected == MessageType::Request;
        case MessageKind::reply:
            return expected == MessageType::Reply ||
                   message.message_class == MessageClass::Error;
        case MessageKind::progress: return expected == MessageType::Progress;
        case MessageKind::error: return expected == MessageType::Error;
        case MessageKind::log: return expected == MessageType::Log;
        case MessageKind::heartbeat: return expected == MessageType::Heartbeat;
        case MessageKind::system: return expected == MessageType::System;
    }
    return false;
}

QoSStats QoSScheduler::snapshot(const ConnectionQueue& connection) {
    const auto stats = connection.queue.stats();
    return QoSStats{stats.enqueued,
                    stats.dequeued,
                    stats.dropped,
                    stats.coalesced,
                    stats.rejected,
                    connection.resource_exhausted,
                    stats.critical_overflow,
                    stats.high_watermark};
}

EnqueueResult QoSScheduler::enqueue(const SessionId& target, RouteMessage message) {
    if (target.empty()) {
        return {EnqueueDisposition::rejected,
                HubStatus::failure(HubErrorCode::invalid_argument,
                                   "QoS target session is required")};
    }
    if (!validSemantics(message)) {
        return {EnqueueDisposition::rejected,
                HubStatus::failure(HubErrorCode::invalid_argument,
                                   "message QoS semantics are inconsistent")};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto [found, inserted] = queues_.try_emplace(target);
    if (inserted) {
        found->second = std::make_unique<ConnectionQueue>(config_.capacity);
    }
    auto& connection = *found->second;
    QueueMetadata metadata;
    metadata.priority = static_cast<QosPriority>(message.priority);
    metadata.message_class = message.message_class;
    metadata.coalescing_key = message.operation_id;
    const auto priority = metadata.priority;
    const auto result = connection.queue.tryEnqueue(std::move(message),
                                                    std::move(metadata));
    switch (result) {
        case ::industrial_bus::EnqueueDisposition::Enqueued:
            return {EnqueueDisposition::accepted, HubStatus::success()};
        case ::industrial_bus::EnqueueDisposition::Coalesced:
            return {EnqueueDisposition::coalesced, HubStatus::success()};
        case ::industrial_bus::EnqueueDisposition::CriticalOverflow:
            return {EnqueueDisposition::disconnect_slow_consumer,
                    HubStatus::failure(HubErrorCode::resource_exhausted,
                                       "critical queue exhausted")};
        case ::industrial_bus::EnqueueDisposition::Rejected:
            if (priority == QosPriority::High) {
                ++connection.resource_exhausted;
                return {EnqueueDisposition::rejected,
                        HubStatus::failure(HubErrorCode::resource_exhausted,
                                           "command/request queue exhausted")};
            }
            return {EnqueueDisposition::dropped, HubStatus::success()};
    }
    return {EnqueueDisposition::rejected,
            HubStatus::failure(HubErrorCode::invalid_argument,
                               "unknown queue result")};
}

std::optional<RouteMessage> QoSScheduler::pop(const SessionId& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = queues_.find(target);
    if (found == queues_.end()) {
        return std::nullopt;
    }
    return found->second->queue.tryDequeue();
}

void QoSScheduler::remove(const SessionId& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    queues_.erase(target);
}

QoSStats QoSScheduler::stats(const SessionId& target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = queues_.find(target);
    return found == queues_.end() ? QoSStats{} : snapshot(*found->second);
}

}  // namespace industrial_bus::commhub
