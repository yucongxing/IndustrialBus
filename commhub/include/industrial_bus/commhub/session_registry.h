#pragma once

#include "industrial_bus/commhub/types.h"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace industrial_bus::commhub {

class SessionRegistry {
public:
    HubStatus add(Session session, SteadyTime now = std::chrono::steady_clock::now());
    bool remove(const SessionId& session_id);
    bool touch(const SessionId& session_id,
               SteadyTime now = std::chrono::steady_clock::now());
    HubStatus subscribe(const SessionId& session_id, std::string topic);
    bool unsubscribe(const SessionId& session_id, const std::string& topic);

    std::optional<Session> find(const SessionId& session_id) const;
    std::vector<Session> snapshot() const;
    std::vector<SessionId> stale(std::chrono::milliseconds timeout,
                                 SteadyTime now = std::chrono::steady_clock::now()) const;
    std::vector<SessionId> subscribers(const std::string& topic,
                                       const SessionId& excluding = {}) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<SessionId, Session> sessions_;
};

}  // namespace industrial_bus::commhub
