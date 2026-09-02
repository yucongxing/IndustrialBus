#include "industrial_bus/commhub/service_registry.h"

#include <algorithm>

namespace industrial_bus::commhub {

HubStatus ServiceRegistry::registerService(const SessionId& session_id,
                                           ServiceRegistration registration) {
    if (session_id.empty() || registration.service_name.empty() ||
        registration.instance_id.empty()) {
        return HubStatus::failure(HubErrorCode::invalid_argument,
                                  "session, service and instance are required");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entries = services_[registration.service_name];
    const auto duplicate = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.registration.instance_id == registration.instance_id ||
               entry.session_id == session_id;
    });
    if (duplicate != entries.end()) {
        return HubStatus::failure(HubErrorCode::service_already_registered,
                                  "service instance is already registered");
    }
    const bool singleton_conflict = std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.registration.instance_mode == InstanceMode::singleton_per_user ||
               registration.instance_mode == InstanceMode::singleton_per_user;
    });
    if (singleton_conflict) {
        return HubStatus::failure(HubErrorCode::service_already_registered,
                                  "singleton service already has an online instance");
    }
    entries.push_back({session_id, std::move(registration)});
    return HubStatus::success();
}

std::size_t ServiceRegistry::unregisterSession(const SessionId& session_id) {
    std::size_t removed = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto service = services_.begin(); service != services_.end();) {
        auto& entries = service->second;
        const auto old_size = entries.size();
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
                          return entry.session_id == session_id;
                      }),
                      entries.end());
        removed += old_size - entries.size();
        if (entries.empty()) {
            service = services_.erase(service);
        } else {
            ++service;
        }
    }
    return removed;
}

std::optional<ServiceInstance> ServiceRegistry::exact(const std::string& service_name,
                                                      const std::string& instance_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto service = services_.find(service_name);
    if (service == services_.end()) {
        return std::nullopt;
    }
    const auto found = std::find_if(service->second.begin(), service->second.end(),
                                    [&](const auto& entry) {
                                        return entry.registration.instance_id == instance_id;
                                    });
    return found == service->second.end() ? std::nullopt
                                          : std::optional<ServiceInstance>(*found);
}

std::vector<ServiceInstance> ServiceRegistry::instances(const std::string& service_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = services_.find(service_name);
    return found == services_.end() ? std::vector<ServiceInstance>{} : found->second;
}

std::vector<ServiceInstance> ServiceRegistry::allInstances() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServiceInstance> result;
    for (const auto& service : services_) {
        result.insert(result.end(), service.second.begin(), service.second.end());
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.registration.service_name != right.registration.service_name) {
            return left.registration.service_name < right.registration.service_name;
        }
        return left.registration.instance_id < right.registration.instance_id;
    });
    return result;
}

std::optional<ServiceInstance> ServiceRegistry::resolve(const std::string& service_name,
                                                        const std::string& instance_id,
                                                        SelectionPolicy policy,
                                                        const std::string& source_node) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto service = services_.find(service_name);
    if (service == services_.end() || service->second.empty()) {
        return std::nullopt;
    }
    if (!instance_id.empty()) {
        const auto found = std::find_if(service->second.begin(), service->second.end(),
                                        [&](const auto& entry) {
                                            return entry.registration.instance_id == instance_id;
                                        });
        return found == service->second.end() ? std::nullopt
                                              : std::optional<ServiceInstance>(*found);
    }
    if (service->second.size() == 1 &&
        service->second.front().registration.instance_mode == InstanceMode::singleton_per_user) {
        return service->second.front();
    }
    if (policy == SelectionPolicy::prefer_local) {
        std::optional<ServiceInstance> local;
        for (const auto& entry : service->second) {
            if (entry.registration.node_id == source_node) {
                if (local) {
                    return std::nullopt;
                }
                local = entry;
            }
        }
        return local;
    }
    return std::nullopt;
}

}  // namespace industrial_bus::commhub
