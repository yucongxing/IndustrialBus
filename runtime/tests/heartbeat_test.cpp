#include "industrial_bus/runtime/commhub_server.h"
#include "industrial_bus/runtime/transport_runtime_adapter.h"
#include "loopback_transport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using industrial_bus::ConnectionState;
using industrial_bus::runtime::CommHubServer;
using industrial_bus::runtime::CommHubServerOptions;
using industrial_bus::runtime::TransportRuntimeAdapter;
using industrial_bus::runtime::TransportRuntimeOptions;
using industrial_bus::transport::Endpoint;
using industrial_bus::transport::Future;
using industrial_bus::transport::IConnection;
using industrial_bus::transport::IConnector;
using industrial_bus::transport::LoopbackHub;
using industrial_bus::transport::Result;

class BlockingConnector final : public IConnector {
public:
    explicit BlockingConnector(std::shared_ptr<IConnector> inner)
        : inner_(std::move(inner)) {}

    Future<Result<std::shared_ptr<IConnection>>> connect(
        const Endpoint& endpoint) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entered_ = true;
        }
        cv_.notify_all();
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return released_; });
        lock.unlock();
        return inner_->connect(endpoint);
    }

    bool waitUntilEntered(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    std::shared_ptr<IConnector> inner_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_{false};
    bool released_{false};
};

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

TransportRuntimeOptions clientOptions(
    const std::shared_ptr<LoopbackHub>& transport,
    const Endpoint& endpoint,
    std::string service,
    std::chrono::milliseconds interval = 20ms,
    std::chrono::milliseconds timeout = 80ms) {
    TransportRuntimeOptions options;
    options.endpoint = endpoint;
    options.connector = transport;
    options.service_name = std::move(service);
    options.instance_id = options.service_name + "-instance";
    options.auth_token = "heartbeat-token";
    options.expected_hub_instance_id = "heartbeat-hub";
    options.heartbeat_interval = interval;
    options.heartbeat_timeout = timeout;
    return options;
}

TEST(RuntimeHeartbeatTest, KeepsLiveSessionAndExpiresQuietSession) {
    auto transport = std::make_shared<LoopbackHub>();
    const Endpoint endpoint{"loopback://heartbeat-expiry"};
    CommHubServerOptions server_options;
    server_options.heartbeat_timeout = 120ms;
    server_options.sweep_interval = 10ms;
    CommHubServer server(transport, endpoint, "heartbeat-hub",
                         "heartbeat-token", server_options);
    ASSERT_TRUE(server.start());

    TransportRuntimeAdapter live(
        clientOptions(transport, endpoint, "live-service"));
    auto quiet_options = clientOptions(transport, endpoint, "quiet-service");
    quiet_options.heartbeat_interval = 0ms;
    TransportRuntimeAdapter quiet(std::move(quiet_options));
    ASSERT_TRUE(live.start());
    ASSERT_TRUE(quiet.start());

    const auto initial =
        server.core().sessions().find("live-service-instance");
    ASSERT_TRUE(initial.has_value());
    ASSERT_TRUE(waitUntil([&] {
        const auto refreshed =
            server.core().sessions().find("live-service-instance");
        return refreshed &&
               refreshed->last_heartbeat > initial->last_heartbeat;
    }, 500ms));
    ASSERT_TRUE(waitUntil([&] {
        return !server.core().sessions().find("quiet-service-instance");
    }, 500ms));
    EXPECT_TRUE(server.core().sessions().find("live-service-instance"));
    EXPECT_TRUE(server.core().services().exact(
        "live-service", "live-service-instance"));
    EXPECT_FALSE(server.core().services().exact(
        "quiet-service", "quiet-service-instance"));
    const auto discovered = live.services();
    EXPECT_TRUE(std::none_of(discovered.begin(), discovered.end(),
                             [](const auto& service) {
                                 return service.instance_id ==
                                        "quiet-service-instance";
                             }));
    EXPECT_EQ(live.state(), ConnectionState::Connected);

    quiet.close();
    live.close();
    server.stop();
}

TEST(RuntimeHeartbeatTest, DetectsHubLossAndNotifiesOnce) {
    auto transport = std::make_shared<LoopbackHub>();
    const Endpoint endpoint{"loopback://heartbeat-loss"};
    CommHubServer server(transport, endpoint, "heartbeat-hub",
                         "heartbeat-token");
    ASSERT_TRUE(server.start());

    TransportRuntimeAdapter client(
        clientOptions(transport, endpoint, "loss-client"));
    std::atomic<int> disconnected{0};
    client.setConnectionHandler(
        [&](ConnectionState state, std::uint64_t) {
            if (state == ConnectionState::Disconnected) {
                ++disconnected;
                throw std::runtime_error("expected test callback failure");
            }
        });
    ASSERT_TRUE(client.start());
    server.stop();

    ASSERT_TRUE(waitUntil([&] {
        return client.state() == ConnectionState::Disconnected;
    }, 500ms));
    EXPECT_EQ(disconnected.load(), 1);
    client.close();
    EXPECT_EQ(disconnected.load(), 1);
}

TEST(RuntimeHeartbeatTest, ConcurrentStartAndCloseEndsDisconnected) {
    auto transport = std::make_shared<LoopbackHub>();
    auto connector = std::make_shared<BlockingConnector>(transport);
    const Endpoint endpoint{"loopback://heartbeat-start-close"};
    CommHubServer server(transport, endpoint, "heartbeat-hub",
                         "heartbeat-token");
    ASSERT_TRUE(server.start());

    auto options = clientOptions(transport, endpoint, "race-client");
    options.connector = connector;
    TransportRuntimeAdapter client(std::move(options));
    auto started = std::async(std::launch::async, [&] { return client.start(); });
    const auto connect_entered = connector->waitUntilEntered(500ms);
    if (!connect_entered) {
        connector->release();
    }
    ASSERT_TRUE(connect_entered);
    auto closed = std::async(std::launch::async, [&] { client.close(); });
    connector->release();

    ASSERT_EQ(started.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(started.get());
    ASSERT_EQ(closed.wait_for(1s), std::future_status::ready);
    closed.get();
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);

    server.stop();
}

TEST(RuntimeHeartbeatTest, ConnectedCallbackCanWaitForCrossThreadClose) {
    auto transport = std::make_shared<LoopbackHub>();
    const Endpoint endpoint{"loopback://heartbeat-callback-close"};
    CommHubServer server(transport, endpoint, "heartbeat-hub",
                         "heartbeat-token");
    ASSERT_TRUE(server.start());

    TransportRuntimeAdapter client(
        clientOptions(transport, endpoint, "callback-close-client"));
    std::mutex events_mutex;
    std::vector<ConnectionState> events;
    client.setConnectionHandler([&](ConnectionState state, std::uint64_t) {
        {
            std::lock_guard<std::mutex> lock(events_mutex);
            events.push_back(state);
        }
        if (state == ConnectionState::Connected) {
            std::thread closer([&] { client.close(); });
            closer.join();
        }
    });

    ASSERT_TRUE(client.start());
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
    {
        std::lock_guard<std::mutex> lock(events_mutex);
        ASSERT_EQ(events.size(), 2U);
        EXPECT_EQ(events[0], ConnectionState::Connected);
        EXPECT_EQ(events[1], ConnectionState::Disconnected);
    }
    server.stop();
}

TEST(RuntimeHeartbeatTest, RequiresCloseBeforeRestartAfterHeartbeatLoss) {
    auto transport = std::make_shared<LoopbackHub>();
    const Endpoint endpoint{"loopback://heartbeat-restart"};
    CommHubServer server(transport, endpoint, "heartbeat-hub",
                         "heartbeat-token");
    ASSERT_TRUE(server.start());

    TransportRuntimeAdapter client(
        clientOptions(transport, endpoint, "restart-client"));
    ASSERT_TRUE(client.start());
    server.stop();
    ASSERT_TRUE(waitUntil([&] {
        return client.state() == ConnectionState::Disconnected;
    }, 500ms));

    const auto restarted_without_close = client.start();
    ASSERT_FALSE(restarted_without_close);
    EXPECT_EQ(restarted_without_close.error().category,
              industrial_bus::ErrorCategory::InvalidArgument);
    client.close();
}

TEST(RuntimeHeartbeatTest, RejectsUnsafeTiming) {
    auto transport = std::make_shared<LoopbackHub>();
    const Endpoint endpoint{"loopback://heartbeat-invalid"};
    auto options = clientOptions(transport, endpoint, "invalid-client");
    options.heartbeat_interval = 50ms;
    options.heartbeat_timeout = 75ms;
    TransportRuntimeAdapter client(std::move(options));
    const auto started = client.start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().category,
              industrial_bus::ErrorCategory::InvalidArgument);
}

}  // namespace
