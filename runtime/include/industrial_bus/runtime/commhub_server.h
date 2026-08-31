#pragma once

#include "industrial_bus/commhub/hub_core.h"
#include "industrial_bus/result.h"
#include "industrial_bus/transport/transport.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace industrial_bus::runtime {

class CommHubServer {
public:
    CommHubServer(std::shared_ptr<transport::IListener> listener,
                  transport::Endpoint endpoint,
                  std::string hub_instance_id = {},
                  std::string auth_token = {});
    ~CommHubServer();

    Result<void> start();
    void stop() noexcept;

    commhub::HubCore& core() noexcept { return core_; }

private:
    struct Peer;
    void accept(std::shared_ptr<transport::IConnection> connection);
    void receive(const std::shared_ptr<Peer>& peer, transport::ByteBuffer frame);
    Result<void> registerPeer(const std::shared_ptr<Peer>& peer,
                              const transport::ByteBuffer& frame);
    Result<void> updateSubscription(const std::shared_ptr<Peer>& peer,
                                    const transport::ByteBuffer& frame);
    void drain(const std::vector<commhub::SessionId>& targets);
    void sendRouteError(const std::shared_ptr<Peer>& peer,
                        const transport::ByteBuffer& request,
                        const commhub::HubError& error);

    std::shared_ptr<transport::IListener> listener_;
    transport::Endpoint endpoint_;
    std::string hub_instance_id_;
    std::string auth_token_;
    commhub::HubCore core_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Peer>> peers_;
    std::unordered_map<commhub::SessionId, std::shared_ptr<Peer>> sessions_;
    bool started_{false};
};

}  // namespace industrial_bus::runtime
