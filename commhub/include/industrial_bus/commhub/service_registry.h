#pragma once

#include "industrial_bus/commhub/types.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace industrial_bus::commhub {

class ServiceRegistry {
public:
    HubStatus registerService(const SessionId& session_id,
                              ServiceRegistration registration);
    std::size_t unregisterSession(const SessionId& session_id);

    std::optional<ServiceInstance> exact(const std::string& service_name,
                                         const std::string& instance_id) const;
    std::vector<ServiceInstance> instances(const std::string& service_name) const;
    std::optional<ServiceInstance> resolve(const std::string& service_name,
                                           const std::string& instance_id,
                                           SelectionPolicy policy,
                                           const std::string& source_node) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<ServiceInstance>> services_;
};

}  // namespace industrial_bus::commhub
