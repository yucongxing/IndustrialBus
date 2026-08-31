#pragma once

#include "industrial_bus/commhub/types.h"

#include <array>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace industrial_bus::commhub {

struct QoSConfig {
    std::array<std::size_t, 4> capacity{{128, 256, 512, 256}};
};

struct QoSStats {
    std::uint64_t enqueued{0};
    std::uint64_t dropped{0};
    std::uint64_t progress_coalesced{0};
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
        std::array<std::deque<RouteMessage>, 4> queues;
        QoSStats stats;
    };

    static std::size_t level(const RouteMessage& message);
    static std::size_t totalSize(const ConnectionQueue& queue);

    QoSConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<SessionId, ConnectionQueue> queues_;
};

}  // namespace industrial_bus::commhub
