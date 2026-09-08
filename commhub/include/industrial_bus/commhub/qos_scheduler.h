#pragma once

#include "industrial_bus/commhub/types.h"
#include "industrial_bus/core/bounded_priority_queue.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace industrial_bus::commhub {

struct QoSConfig {
    // One aggregate bound per destination connection. 1152 preserves the
    // approximate total capacity of the former per-priority defaults.
    std::size_t capacity{1152};
};

struct QoSStats {
    std::uint64_t enqueued{0};
    std::uint64_t dequeued{0};
    std::uint64_t dropped{0};
    std::uint64_t progress_coalesced{0};
    std::uint64_t rejected{0};
    std::uint64_t resource_exhausted{0};
    std::uint64_t critical_overflow{0};
    std::size_t high_watermark{0};
};

enum class EnqueueDisposition {
    accepted,
    coalesced,
    dropped,
    rejected,
    disconnect_slow_consumer,
};

struct EnqueueResult {
    EnqueueDisposition disposition{EnqueueDisposition::accepted};
    HubStatus status;

    explicit operator bool() const noexcept {
        return disposition == EnqueueDisposition::accepted ||
               disposition == EnqueueDisposition::coalesced;
    }
};

class QoSScheduler {
public:
    explicit QoSScheduler(QoSConfig config = {});

    EnqueueResult enqueue(const SessionId& target, RouteMessage message);
    std::optional<RouteMessage> pop(const SessionId& target);
    void remove(const SessionId& target);
    QoSStats stats(const SessionId& target) const;

private:
    struct ConnectionQueue {
        explicit ConnectionQueue(std::size_t capacity) : queue(capacity) {}

        ::industrial_bus::BoundedPriorityQueue<RouteMessage> queue;
        std::uint64_t resource_exhausted{0};
    };

    static bool validSemantics(const RouteMessage& message);
    static QoSStats snapshot(const ConnectionQueue& connection);

    QoSConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<SessionId, std::unique_ptr<ConnectionQueue>> queues_;
};

}  // namespace industrial_bus::commhub
