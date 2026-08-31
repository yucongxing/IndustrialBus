#include "industrial_bus/commhub/return_route_table.h"

#include <algorithm>

namespace industrial_bus::commhub {

ReturnRouteTable::ReturnRouteTable(std::chrono::milliseconds maximum_ttl)
    : maximum_ttl_(maximum_ttl) {}

HubStatus ReturnRouteTable::put(std::string request_message_id,
                                SessionId source_session,
                                SessionId destination_session,
                                std::int64_t deadline_unix_ns,
                                SteadyTime now,
                                std::int64_t now_unix_ns) {
    if (request_message_id.empty() || source_session.empty() || destination_session.empty()) {
        return HubStatus::failure(HubErrorCode::invalid_argument,
                                  "return route requires request and session ids");
    }
    if (deadline_unix_ns <= now_unix_ns) {
        return HubStatus::failure(HubErrorCode::deadline_expired,
                                  "request deadline already expired");
    }
    const auto until_deadline = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(deadline_unix_ns - now_unix_ns));
    const auto ttl = std::min(maximum_ttl_, until_deadline);
    ReturnRoute route{std::move(request_message_id), std::move(source_session),
                      std::move(destination_session), deadline_unix_ns, now + ttl};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto inserted = routes_.emplace(route.request_message_id, std::move(route));
    if (!inserted.second) {
        return HubStatus::failure(HubErrorCode::invalid_argument,
                                  "request message id already has a return route");
    }
    return HubStatus::success();
}

std::optional<ReturnRoute> ReturnRouteTable::take(const std::string& request_message_id,
                                                  const SessionId& destination_session,
                                                  SteadyTime now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = routes_.find(request_message_id);
    if (found == routes_.end()) {
        return std::nullopt;
    }
    if (found->second.expires_at <= now) {
        routes_.erase(found);
        return std::nullopt;
    }
    if (found->second.destination_session != destination_session) {
        return std::nullopt;
    }
    ReturnRoute route = std::move(found->second);
    routes_.erase(found);
    return route;
}

bool ReturnRouteTable::erase(const std::string& request_message_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return routes_.erase(request_message_id) != 0;
}

std::size_t ReturnRouteTable::purge(SteadyTime now) {
    std::size_t removed = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto route = routes_.begin(); route != routes_.end();) {
        if (route->second.expires_at <= now) {
            route = routes_.erase(route);
            ++removed;
        } else {
            ++route;
        }
    }
    return removed;
}

std::size_t ReturnRouteTable::eraseForSession(const SessionId& session_id) {
    std::size_t removed = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto route = routes_.begin(); route != routes_.end();) {
        if (route->second.source_session == session_id ||
            route->second.destination_session == session_id) {
            route = routes_.erase(route);
            ++removed;
        } else {
            ++route;
        }
    }
    return removed;
}

std::size_t ReturnRouteTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return routes_.size();
}

}  // namespace industrial_bus::commhub
