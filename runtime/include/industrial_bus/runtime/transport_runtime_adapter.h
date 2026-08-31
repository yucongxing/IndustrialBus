#pragma once

#include "industrial_bus/client/runtime_adapter.h"
#include "industrial_bus/transport/transport.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace industrial_bus::runtime {

struct TransportRuntimeOptions {
    transport::Endpoint endpoint;
    std::shared_ptr<transport::IConnector> connector;
    std::string node_id{"local"};
    std::string service_name;
    std::string instance_id;
    std::string service_version{"0.1.0"};
    std::string auth_token;
    std::chrono::milliseconds handshake_timeout{std::chrono::seconds(3)};
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
    bool flush(std::chrono::milliseconds timeout) override;
    void close() noexcept override;

private:
    Result<void> sendRegistration();
    Result<void> sendSubscription(const std::string& topic, bool subscribe_value);
    void receive(transport::ByteBuffer frame);

    TransportRuntimeOptions options_;
    std::shared_ptr<transport::IConnection> connection_;
    mutable std::mutex mutex_;
    std::condition_variable registration_cv_;
    ReceiveHandler receive_handler_;
    ConnectionHandler connection_handler_;
    ConnectionState state_{ConnectionState::Disconnected};
    std::uint64_t generation_{0};
    bool registered_{false};
};

}  // namespace industrial_bus::runtime
