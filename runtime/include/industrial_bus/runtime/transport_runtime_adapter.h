#pragma once

#include "industrial_bus/client/runtime_adapter.h"
#include "industrial_bus/transport/transport.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace industrial_bus::runtime {

struct TransportRuntimeOptions {
    transport::Endpoint endpoint;
    std::shared_ptr<transport::IConnector> connector;
    std::string node_id{"local"};
    std::string service_name;
    std::string instance_id;
    std::string service_version{"0.1.0"};
    InstanceMode instance_mode{InstanceMode::SingletonPerUser};
    std::string auth_token;
    std::string expected_hub_instance_id;
    std::uint32_t expected_protocol_version{1};
    std::chrono::milliseconds handshake_timeout{std::chrono::seconds(3)};
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(2)};
    std::chrono::milliseconds heartbeat_timeout{std::chrono::seconds(6)};
};

class TransportRuntimeAdapter final : public client::IRuntimeAdapter {
public:
    explicit TransportRuntimeAdapter(TransportRuntimeOptions options);
    ~TransportRuntimeAdapter() override;

    Result<void> start() override;
    Result<void> send(client::OutboundMessage message) override;
    Result<void> subscribe(const std::string& topic) override;
    Result<void> unsubscribe(const std::string& topic) override;
    void setReceiveHandler(ReceiveHandler handler) override;
    void setConnectionHandler(ConnectionHandler handler) override;
    ConnectionState state() const override;
    std::uint64_t generation() const override;
    ServiceSnapshot services() const override;
    ServiceInfo localService() const override;
    bool flush(std::chrono::milliseconds timeout) override;
    void close() noexcept override;

private:
    Result<void> sendRegistration();
    void sendDeregistration(
        const std::shared_ptr<transport::IConnection>& connection) noexcept;
    Result<void> sendSubscription(const std::string& topic, bool subscribe_value);
    Result<void> sendHeartbeat(const std::shared_ptr<transport::IConnection>& connection,
                               const std::string& message_id,
                               std::uint64_t sequence);
    void heartbeatLoop();
    bool disconnectFromHeartbeat(bool require_timeout) noexcept;
    void receive(transport::ByteBuffer frame);

    std::recursive_mutex lifecycle_mutex_;
    TransportRuntimeOptions options_;
    std::shared_ptr<transport::IConnection> connection_;
    mutable std::mutex mutex_;
    std::condition_variable registration_cv_;
    std::condition_variable heartbeat_cv_;
    std::thread heartbeat_thread_;
    ReceiveHandler receive_handler_;
    ConnectionHandler connection_handler_;
    ConnectionState state_{ConnectionState::Disconnected};
    std::uint64_t generation_{0};
    bool registered_{false};
    std::string registration_message_id_;
    std::optional<Error> registration_error_;
    ServiceSnapshot services_;
    std::uint64_t directory_revision_{0};
    std::chrono::steady_clock::time_point last_hub_activity_{};
    std::uint64_t next_heartbeat_sequence_{1};
    std::unordered_map<std::string, std::uint64_t> pending_heartbeats_;
    std::deque<std::string> heartbeat_order_;
    bool heartbeat_stop_{false};
    bool startup_complete_{true};
    bool connected_notification_in_progress_{false};
    bool disconnected_notification_pending_{false};
    std::uint64_t pending_disconnected_generation_{0};
    bool heartbeat_disconnect_callback_active_{false};
};

}  // namespace industrial_bus::runtime
