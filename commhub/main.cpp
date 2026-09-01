#include "industrial_bus/bootstrap/endpoint_record.h"
#include "industrial_bus/bootstrap/hub_lock.h"
#include "industrial_bus/bootstrap/runtime_paths.h"
#include "industrial_bus/ids.h"
#include "industrial_bus/runtime/commhub_server.h"
#include "zeromq_transport.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

std::atomic<bool> stop_requested{false};

extern "C" void requestStop(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

std::int64_t unixNowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<int> inheritedLockDescriptor(int argc, char** argv) {
    if (argc == 1) {
        return std::nullopt;
    }
    if (argc != 2) {
        throw std::invalid_argument("usage: commhub [--startup-lock-fd=N]");
    }
    constexpr std::string_view prefix = "--startup-lock-fd=";
    const std::string_view argument(argv[1]);
    if (argument.substr(0, prefix.size()) != prefix) {
        throw std::invalid_argument("unknown CommHub argument");
    }
    int descriptor = -1;
    const auto number = argument.substr(prefix.size());
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(),
                                        descriptor);
    if (number.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != number.data() + number.size() || descriptor < 0) {
        throw std::invalid_argument("invalid --startup-lock-fd value");
    }
    return descriptor;
}

class RuntimeArtifactGuard {
public:
    RuntimeArtifactGuard(const industrial_bus::bootstrap::RuntimePaths& paths,
                         std::string hub_instance_id)
        : paths_(paths), hub_instance_id_(std::move(hub_instance_id)) {}

    void arm() noexcept { armed_ = true; }
    void published() noexcept { published_ = true; }

    ~RuntimeArtifactGuard() {
        if (!armed_) {
            return;
        }
        if (published_) {
            try {
                const auto record =
                    industrial_bus::bootstrap::readEndpointRecord(paths_);
                if (record.hub_instance_id != hub_instance_id_) {
                    return;
                }
            } catch (const std::exception&) {
                return;
            }
        }
        ::unlink(paths_.endpointRecordPath().c_str());
        ::unlink(paths_.tokenPath().c_str());
        ::unlink(paths_.socketPath().c_str());
    }

private:
    const industrial_bus::bootstrap::RuntimePaths& paths_;
    std::string hub_instance_id_;
    bool armed_{false};
    bool published_{false};
};

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto inherited_fd = inheritedLockDescriptor(argc, argv);
        const auto paths =
            industrial_bus::bootstrap::RuntimePaths::fromEnvironment();
        std::optional<industrial_bus::bootstrap::HubLock> lock;
        if (inherited_fd) {
            lock.emplace(industrial_bus::bootstrap::HubLock::adoptInherited(
                paths, *inherited_fd));
        } else {
            lock = industrial_bus::bootstrap::HubLock::tryAcquire(paths);
        }
        if (!lock) {
            std::cerr << "another CommHub instance already owns "
                      << paths.lockPath() << '\n';
            return 2;
        }

        // The exclusive lock proves there is no live owner. Remove the complete
        // stale discovery generation before publishing a new token and endpoint.
        ::unlink(paths.socketPath().c_str());
        ::unlink(paths.endpointRecordPath().c_str());
        ::unlink(paths.tokenPath().c_str());

        const auto hub_instance_id = industrial_bus::generateUuidV4();
        RuntimeArtifactGuard artifacts(paths, hub_instance_id);
        artifacts.arm();
        const auto token = industrial_bus::bootstrap::generateAuthToken();
        industrial_bus::bootstrap::writeAuthToken(paths, token);

        auto listener =
            std::make_shared<industrial_bus::transport::ZeroMqRouterListener>();
        industrial_bus::runtime::CommHubServer server(
            listener, {paths.endpoint()}, hub_instance_id, token);
        const auto started = server.start();
        if (!started) {
            std::cerr << "failed to start CommHub: "
                      << started.error().message << '\n';
            return 3;
        }

        industrial_bus::bootstrap::writeEndpointRecord(
            paths,
            {paths.endpoint(), static_cast<std::int64_t>(::getpid()),
             hub_instance_id, 1, unixNowSeconds()});
        artifacts.published();

        std::signal(SIGINT, requestStop);
        std::signal(SIGTERM, requestStop);
        while (!stop_requested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        server.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CommHub startup failed: " << error.what() << '\n';
        return 1;
    }
}
