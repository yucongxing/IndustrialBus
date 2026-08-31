#pragma once

#include "industrial_bus/payload_codec.h"
#include "industrial_bus/result.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace industrial_bus::client {

class PendingRequestTable {
public:
    using Clock = std::chrono::steady_clock;
    using Completion = std::function<void(Result<ByteBuffer>)>;

    PendingRequestTable();
    ~PendingRequestTable();

    PendingRequestTable(const PendingRequestTable&) = delete;
    PendingRequestTable& operator=(const PendingRequestTable&) = delete;

    bool add(std::string message_id,
             std::string expected_payload_type,
             Clock::time_point deadline,
             Completion completion);
    bool complete(const std::string& message_id,
                  const std::string& received_payload_type,
                  Result<ByteBuffer> result);
    void failAll(const Error& error);
    void shutdown(const Error& error) noexcept;

    std::size_t size() const;
    std::uint64_t lateReplyCount() const noexcept;

private:
    struct Entry {
        std::string expected_payload_type;
        Clock::time_point deadline;
        Completion completion;
    };

    void timerLoop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Entry> entries_;
    std::multimap<Clock::time_point, std::string> deadlines_;
    bool stopping_{false};
    std::thread timer_;
    std::uint64_t late_replies_{0};
};

}  // namespace industrial_bus::client
