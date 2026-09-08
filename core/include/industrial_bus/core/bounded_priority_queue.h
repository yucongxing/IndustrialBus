#pragma once

#include "industrial_bus/qos.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace industrial_bus {

struct QueueStats {
    std::uint64_t enqueued{0};
    std::uint64_t dequeued{0};
    std::uint64_t coalesced{0};
    std::uint64_t dropped{0};
    std::uint64_t rejected{0};
    std::uint64_t critical_overflow{0};
    std::size_t high_watermark{0};
};

template <typename T>
class BoundedPriorityQueue {
public:
    explicit BoundedPriorityQueue(std::size_t capacity) : capacity_(capacity) {}

    BoundedPriorityQueue(const BoundedPriorityQueue&) = delete;
    BoundedPriorityQueue& operator=(const BoundedPriorityQueue&) = delete;

    EnqueueDisposition tryEnqueue(T value, QueueMetadata metadata) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capacity_ == 0) {
            return reject(metadata.message_class);
        }

        if (metadata.message_class == MessageClass::Progress &&
            !metadata.coalescing_key.empty() &&
            coalesceProgress(value, metadata)) {
            ++stats_.coalesced;
            return EnqueueDisposition::Coalesced;
        }

        if (size_ == capacity_) {
            if (!evictDroppableFor(metadata.message_class)) {
                return reject(metadata.message_class);
            }
        }

        const auto priority = indexOf(metadata.priority);
        queues_[priority].push_back(Entry{std::move(value), std::move(metadata)});
        ++size_;
        ++stats_.enqueued;
        if (size_ > stats_.high_watermark) {
            stats_.high_watermark = size_;
        }
        return EnqueueDisposition::Enqueued;
    }

    std::optional<T> tryDequeue() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& queue : queues_) {
            if (!queue.empty()) {
                T value = std::move(queue.front().value);
                queue.pop_front();
                --size_;
                ++stats_.dequeued;
                return value;
            }
        }
        return std::nullopt;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    std::size_t capacity() const noexcept { return capacity_; }

    bool empty() const { return size() == 0; }

    QueueStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    struct Entry {
        T value;
        QueueMetadata metadata;
    };

    static constexpr std::size_t kPriorityCount = 4;

    static constexpr std::size_t indexOf(QosPriority priority) noexcept {
        return static_cast<std::size_t>(priority);
    }

    static constexpr int discardRank(MessageClass message_class) noexcept {
        switch (message_class) {
            case MessageClass::DebugLog: return 0;
            case MessageClass::InfoLog: return 1;
            case MessageClass::Progress: return 2;
            case MessageClass::Event: return 3;
            case MessageClass::ErrorLog:
            case MessageClass::WarnLog: return 4;
            case MessageClass::Reply:
            case MessageClass::Accepted:
            case MessageClass::FinalState:
            case MessageClass::Error:
            case MessageClass::Command:
            case MessageClass::Request:
            case MessageClass::Heartbeat:
            case MessageClass::ServiceState:
                return -1;
        }
        return -1;
    }

    static constexpr int incomingRank(MessageClass message_class) noexcept {
        switch (message_class) {
            case MessageClass::DebugLog: return 0;
            case MessageClass::InfoLog: return 1;
            case MessageClass::Progress: return 2;
            case MessageClass::Event: return 3;
            case MessageClass::ErrorLog:
            case MessageClass::WarnLog: return 4;
            case MessageClass::ServiceState: return 5;
            case MessageClass::Heartbeat:
            case MessageClass::Command:
            case MessageClass::Request: return 6;
            case MessageClass::Reply:
            case MessageClass::Accepted:
            case MessageClass::FinalState:
            case MessageClass::Error: return 7;
        }
        return 0;
    }

    bool coalesceProgress(T& value, const QueueMetadata& metadata) {
        for (auto& queue : queues_) {
            for (auto& entry : queue) {
                if (entry.metadata.message_class == MessageClass::Progress &&
                    entry.metadata.coalescing_key == metadata.coalescing_key) {
                    entry.value = std::move(value);
                    return true;
                }
            }
        }
        return false;
    }

    bool evictDroppableFor(MessageClass incoming_class) {
        const int incoming_rank = incomingRank(incoming_class);
        int candidate_rank = incoming_rank;
        typename std::deque<Entry>::iterator candidate;
        std::size_t candidate_queue = kPriorityCount;

        // Inspect low priority queues first. A candidate must be strictly more disposable
        // than the incoming item; accepted Command/Request entries are never evicted.
        for (std::size_t queue_index = kPriorityCount; queue_index-- > 0;) {
            auto& queue = queues_[queue_index];
            for (auto iterator = queue.begin(); iterator != queue.end(); ++iterator) {
                const int rank = discardRank(iterator->metadata.message_class);
                if (rank >= 0 && rank < candidate_rank) {
                    candidate_rank = rank;
                    candidate = iterator;
                    candidate_queue = queue_index;
                    if (rank == 0) {
                        break;
                    }
                }
            }
            if (candidate_rank == 0) {
                break;
            }
        }

        if (candidate_queue == kPriorityCount) {
            return false;
        }
        queues_[candidate_queue].erase(candidate);
        --size_;
        ++stats_.dropped;
        return true;
    }

    EnqueueDisposition reject(MessageClass message_class) {
        ++stats_.rejected;
        if (defaultPriority(message_class) == QosPriority::Critical) {
            ++stats_.critical_overflow;
            return EnqueueDisposition::CriticalOverflow;
        }
        if (discardRank(message_class) >= 0) {
            ++stats_.dropped;
        }
        return EnqueueDisposition::Rejected;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::array<std::deque<Entry>, kPriorityCount> queues_;
    std::size_t size_{0};
    QueueStats stats_;
};

}  // namespace industrial_bus
