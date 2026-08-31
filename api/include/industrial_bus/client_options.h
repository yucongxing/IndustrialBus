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

struct ClientOptions {
    std::string service_name;
    HubStartupMode startup_mode{HubStartupMode::AutoStart};
    std::size_t callback_threads{1};
    std::size_t outbound_capacity{1024};
    std::shared_ptr<client::IRuntimeAdapter> runtime_adapter;
};

struct ServiceInfo {
    std::string service_name;
    std::string instance_id;
    bool local{true};
};

using ServiceSnapshot = std::vector<ServiceInfo>;

}  // namespace industrial_bus
