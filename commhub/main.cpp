#include "industrial_bus/bootstrap/endpoint_record.h"
#include "industrial_bus/bootstrap/hub_lock.h"
#include "industrial_bus/bootstrap/runtime_paths.h"
#include "industrial_bus/ids.h"
#include "industrial_bus/runtime/commhub_server.h"
#include "zeromq_transport.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
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

}  // namespace

int main() {
    try {
        const auto paths =
            industrial_bus::bootstrap::RuntimePaths::fromEnvironment();
        auto lock = industrial_bus::bootstrap::HubLock::tryAcquire(paths);
        if (!lock) {
            std::cerr << "another CommHub instance already owns "
                      << paths.lockPath() << '\n';
            return 2;
        }

        // The exclusive lock proves there is no live owner. Remove only the
        // stale socket path before binding; control files are atomically replaced.
        ::unlink(paths.socketPath().c_str());

        const auto token = industrial_bus::bootstrap::generateAuthToken();
        industrial_bus::bootstrap::writeAuthToken(paths, token);
        const auto hub_instance_id = industrial_bus::generateUuidV4();

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

        std::signal(SIGINT, requestStop);
        std::signal(SIGTERM, requestStop);
        while (!stop_requested.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        server.stop();
        ::unlink(paths.endpointRecordPath().c_str());
        ::unlink(paths.tokenPath().c_str());
        ::unlink(paths.socketPath().c_str());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CommHub startup failed: " << error.what() << '\n';
        return 1;
    }
}
