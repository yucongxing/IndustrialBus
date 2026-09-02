#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace industrial_bus {
namespace client {
class IRuntimeAdapter;
}
namespace core {
class IdempotencyJournal;
}

using Duration = std::chrono::milliseconds;

enum class HubStartupMode {
    AutoStart,
    ConnectOnly,
    EmbeddedForTest,
};

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    ShuttingDown,
};

enum class InstanceMode {
    SingletonPerUser,
    MultiInstance,
};

struct ClientOptions {
    std::string service_name;
    std::string instance_id;
    std::string node_id{"local"};
    std::string service_version{"0.1.0"};
    InstanceMode instance_mode{InstanceMode::SingletonPerUser};
    HubStartupMode startup_mode{HubStartupMode::AutoStart};
    Duration startup_timeout{std::chrono::seconds(5)};
    Duration heartbeat_interval{std::chrono::seconds(2)};
    Duration heartbeat_timeout{std::chrono::seconds(6)};
    std::string hub_executable;
    std::size_t callback_threads{1};
    std::size_t outbound_capacity{1024};
    std::shared_ptr<client::IRuntimeAdapter> runtime_adapter;
    // Required only when this process registers Command handlers.
    std::shared_ptr<core::IdempotencyJournal> idempotency_journal;
};

struct ServiceInfo {
    std::string service_name;
    std::string instance_id;
    std::string node_id;
    std::string version;
    bool local{false};
};

using ServiceSnapshot = std::vector<ServiceInfo>;

}  // namespace industrial_bus
