#include "industrial_bus/bootstrap/endpoint_record.h"
#include "industrial_bus/bootstrap/runtime_paths.h"
#include "industrial_bus/bus_client.h"
#include "zeromq_transport.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

class RuntimeDirectory {
public:
    RuntimeDirectory() {
        char pattern[] = "/tmp/industrial-bus-autostart-XXXXXX";
        char* created = ::mkdtemp(pattern);
        if (!created) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
        if (::chmod(path_.c_str(), 0700) != 0 ||
            ::setenv("XDG_RUNTIME_DIR", path_.c_str(), 1) != 0) {
            throw std::runtime_error("prepare XDG_RUNTIME_DIR failed");
        }
    }

    ~RuntimeDirectory() {
        ::unsetenv("XDG_RUNTIME_DIR");
        const std::string runtime = path_ + "/industrial-bus";
        ::unlink((runtime + "/endpoint.json").c_str());
        ::unlink((runtime + "/auth.token").c_str());
        ::unlink((runtime + "/commhub.sock").c_str());
        ::unlink((runtime + "/commhub.lock").c_str());
        ::rmdir(runtime.c_str());
        ::rmdir(path_.c_str());
    }

private:
    std::string path_;
};

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

}  // namespace

TEST(LinuxClientStartTest, HonorsConnectOnlyAndAutoStartsCommHub) {
    const int socket_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0 && (errno == EPERM || errno == EACCES)) {
        GTEST_SKIP() << "sandbox does not allow Unix sockets";
    }
    ASSERT_GE(socket_fd, 0) << "Unix socket preflight failed";
    ::close(socket_fd);

    RuntimeDirectory runtime_directory;
    const auto paths =
        industrial_bus::bootstrap::RuntimePaths::fromEnvironment();
    industrial_bus::transport::ZeroMqRouterListener preflight;
    const industrial_bus::transport::Endpoint preflight_endpoint{
        "ipc://" + paths.runtimeDir() + "/preflight.sock"};
    const auto listening = preflight.listen(
        preflight_endpoint,
        [](std::shared_ptr<industrial_bus::transport::IConnection>) {});
    if (!listening &&
        listening.error().message.find("Operation not permitted") !=
            std::string::npos) {
        GTEST_SKIP() << "sandbox does not allow ZeroMQ IPC";
    }
    ASSERT_TRUE(listening) << listening.error().message;
    preflight.close();
    ::unlink((paths.runtimeDir() + "/preflight.sock").c_str());

    industrial_bus::ClientOptions connect_only;
    connect_only.service_name = "connect-only-before-start";
    connect_only.startup_mode = industrial_bus::HubStartupMode::ConnectOnly;
    connect_only.startup_timeout = std::chrono::milliseconds(200);
    auto absent = industrial_bus::BusClient::connect(connect_only);
    EXPECT_FALSE(absent);
    EXPECT_NE(::access(paths.endpointRecordPath().c_str(), F_OK), 0);

    industrial_bus::ClientOptions auto_start;
    auto_start.service_name = "autostart-client";
    auto_start.startup_mode = industrial_bus::HubStartupMode::AutoStart;
    auto_start.startup_timeout = std::chrono::seconds(4);
    auto_start.hub_executable = INDUSTRIAL_BUS_TEST_COMMHUB_PATH;
    auto connected = industrial_bus::BusClient::connect(auto_start);
    ASSERT_TRUE(connected) << connected.error().message;
    auto first_bus = connected.value();
    const auto endpoint = industrial_bus::bootstrap::readEndpointRecord(paths);
    EXPECT_GT(endpoint.pid, 0);
    EXPECT_FALSE(endpoint.hub_instance_id.empty());
    EXPECT_EQ(endpoint.protocol_version, 1U);
    EXPECT_EQ(industrial_bus::bootstrap::readAuthToken(paths).size(), 64U);
    ASSERT_TRUE(first_bus->shutdown(std::chrono::milliseconds(100)));

    industrial_bus::ClientOptions existing_hub;
    existing_hub.service_name = "connect-only-after-start";
    existing_hub.startup_mode = industrial_bus::HubStartupMode::ConnectOnly;
    existing_hub.startup_timeout = std::chrono::seconds(2);
    auto reconnected = industrial_bus::BusClient::connect(existing_hub);
    ASSERT_TRUE(reconnected) << reconnected.error().message;
    ASSERT_TRUE(reconnected.value()->shutdown(std::chrono::milliseconds(100)));

    ASSERT_EQ(::kill(static_cast<pid_t>(endpoint.pid), SIGTERM), 0);
    EXPECT_TRUE(waitUntil([&] {
        return ::access(paths.endpointRecordPath().c_str(), F_OK) != 0 &&
               ::access(paths.tokenPath().c_str(), F_OK) != 0 &&
               ::access(paths.socketPath().c_str(), F_OK) != 0;
    }, std::chrono::seconds(3))) << "auto-started CommHub did not clean up";
}
