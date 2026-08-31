#pragma once

#include "industrial_bus/commhub/types.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace industrial_bus::commhub {

class ReturnRouteTable {
public:
    explicit ReturnRouteTable(std::chrono::milliseconds maximum_ttl =
                                  std::chrono::seconds(30));

    HubStatus put(std::string request_message_id,
                  SessionId source_session,
                  SessionId destination_session,
                  std::int64_t deadline_unix_ns,
                  SteadyTime now,
                  std::int64_t now_unix_ns);

    // A reply consumes the one-shot route. Expired routes disappear silently;
    // the caller SDK, not the Hub, owns the user-visible timeout.
    std::optional<ReturnRoute> take(const std::string& request_message_id,
                                    const SessionId& destination_session,
                                    SteadyTime now = std::chrono::steady_clock::now());
    bool erase(const std::string& request_message_id);
    std::size_t purge(SteadyTime now = std::chrono::steady_clock::now());
    std::size_t eraseForSession(const SessionId& session_id);
    std::size_t size() const;

private:
    std::chrono::milliseconds maximum_ttl_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ReturnRoute> routes_;
};

}  // namespace industrial_bus::commhub
