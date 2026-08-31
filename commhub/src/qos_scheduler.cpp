#include "industrial_bus/commhub/qos_scheduler.h"

#include <algorithm>

namespace industrial_bus::commhub {

QoSScheduler::QoSScheduler(QoSConfig config) : config_(config) {}

std::size_t QoSScheduler::level(const RouteMessage& message) {
    if (message.kind == MessageKind::reply || message.kind == MessageKind::error ||
        message.priority == 0) {
        return 0;
    }
    if (message.kind == MessageKind::command || message.kind == MessageKind::request ||
        message.kind == MessageKind::system || message.priority == 1) {
        return 1;
    }
    return std::min<std::size_t>(message.priority, 3);
}

std::size_t QoSScheduler::totalSize(const ConnectionQueue& queue) {
    std::size_t total = 0;
    for (const auto& level_queue : queue.queues) {
        total += level_queue.size();
    }
    return total;
}

EnqueueResult QoSScheduler::enqueue(const SessionId& target, RouteMessage message) {
    if (target.empty()) {
        return {EnqueueDisposition::rejected,
                HubStatus::failure(HubErrorCode::invalid_argument,
                                   "QoS target session is required")};
    }
    const auto priority = level(message);
    std::lock_guard<std::mutex> lock(mutex_);
    auto& connection = queues_[target];

    if (message.kind == MessageKind::progress && !message.operation_id.empty()) {
        auto& queue = connection.queues[priority];
        const auto existing = std::find_if(queue.begin(), queue.end(), [&](const auto& queued) {
            return queued.kind == MessageKind::progress &&
                   queued.operation_id == message.operation_id;
        });
        if (existing != queue.end()) {
            *existing = std::move(message);
            ++connection.stats.progress_coalesced;
            return {EnqueueDisposition::coalesced, HubStatus::success()};
        }
    }

    auto& queue = connection.queues[priority];
    if (queue.size() >= config_.capacity[priority]) {
        if (priority == 0) {
            ++connection.stats.critical_overflow;
            return {EnqueueDisposition::disconnect_slow_consumer,
                    HubStatus::failure(HubErrorCode::resource_exhausted,
                                       "critical queue exhausted")};
        } else if (priority == 1) {
            ++connection.stats.resource_exhausted;
            return {EnqueueDisposition::rejected,
                    HubStatus::failure(HubErrorCode::resource_exhausted,
                                       "command/request queue exhausted")};
        } else {
            ++connection.stats.dropped;
            return {EnqueueDisposition::dropped, HubStatus::success()};
        }
    }
    queue.push_back(std::move(message));
    ++connection.stats.enqueued;
    connection.stats.high_watermark =
        std::max(connection.stats.high_watermark, totalSize(connection));
    return {EnqueueDisposition::accepted, HubStatus::success()};
}

std::optional<RouteMessage> QoSScheduler::pop(const SessionId& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = queues_.find(target);
    if (found == queues_.end()) {
        return std::nullopt;
    }
    for (auto& queue : found->second.queues) {
        if (!queue.empty()) {
            RouteMessage message = std::move(queue.front());
            queue.pop_front();
            return message;
        }
    }
    return std::nullopt;
}

void QoSScheduler::remove(const SessionId& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    queues_.erase(target);
}

QoSStats QoSScheduler::stats(const SessionId& target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = queues_.find(target);
    return found == queues_.end() ? QoSStats{} : found->second.stats;
}

}  // namespace industrial_bus::commhub
