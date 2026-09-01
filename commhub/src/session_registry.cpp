#include "industrial_bus/commhub/session_registry.h"

namespace industrial_bus::commhub {

HubStatus SessionRegistry::add(Session session, SteadyTime now) {
    if (session.id.empty()) {
        return HubStatus::failure(HubErrorCode::invalid_argument, "session id is required");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.find(session.id) != sessions_.end()) {
        return HubStatus::failure(HubErrorCode::invalid_argument,
                                  "session id is already registered");
    }
    session.registered_at = now;
    session.last_heartbeat = now;
    sessions_.emplace(session.id, std::move(session));
    return HubStatus::success();
}

bool SessionRegistry::remove(const SessionId& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.erase(session_id) != 0;
}

bool SessionRegistry::touch(const SessionId& session_id, SteadyTime now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(session_id);
    if (found == sessions_.end()) {
        return false;
    }
    found->second.last_heartbeat = now;
    return true;
}

HubStatus SessionRegistry::subscribe(const SessionId& session_id, std::string topic) {
    if (topic.empty()) {
        return HubStatus::failure(HubErrorCode::invalid_argument,
                                  "subscription topic is required");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(session_id);
    if (found == sessions_.end()) {
        return HubStatus::failure(HubErrorCode::session_not_found,
                                  "cannot subscribe an unknown session");
    }
    found->second.subscriptions.insert(std::move(topic));
    return HubStatus::success();
}

bool SessionRegistry::unsubscribe(const SessionId& session_id, const std::string& topic) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(session_id);
    return found != sessions_.end() && found->second.subscriptions.erase(topic) != 0;
}

std::optional<Session> SessionRegistry::find(const SessionId& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(session_id);
    return found == sessions_.end() ? std::nullopt : std::optional<Session>(found->second);
}

std::vector<Session> SessionRegistry::snapshot() const {
    std::vector<Session> sessions;
    std::lock_guard<std::mutex> lock(mutex_);
    sessions.reserve(sessions_.size());
    for (const auto& entry : sessions_) {
        sessions.push_back(entry.second);
    }
    return sessions;
}

std::vector<SessionId> SessionRegistry::removeStale(
    std::chrono::milliseconds timeout,
    SteadyTime now) {
    std::vector<SessionId> sessions;
    if (timeout <= std::chrono::milliseconds::zero()) {
        return sessions;
    }
    const auto cutoff = now - timeout;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto entry = sessions_.begin(); entry != sessions_.end();) {
        if (entry->second.last_heartbeat <= cutoff) {
            sessions.push_back(entry->first);
            entry = sessions_.erase(entry);
        } else {
            ++entry;
        }
    }
    return sessions;
}

std::vector<SessionId> SessionRegistry::subscribers(const std::string& topic,
                                                    const SessionId& excluding) const {
    std::vector<SessionId> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : sessions_) {
        if (entry.first != excluding && entry.second.subscriptions.count(topic) != 0) {
            result.push_back(entry.first);
        }
    }
    return result;
}

}  // namespace industrial_bus::commhub
