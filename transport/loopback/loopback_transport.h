#pragma once

#include "industrial_bus/transport/transport.h"

#include <mutex>
#include <string>
#include <vector>

namespace industrial_bus::transport {

// An in-process listener/connector pair. Every connect creates two independent
// IConnection endpoints; sends copy the frame before invoking the peer.
class LoopbackHub final : public IConnector, public IListener {
public:
    LoopbackHub() = default;
    ~LoopbackHub() override;

    Result<void> listen(const Endpoint& endpoint, AcceptHandler handler) override;
    Future<Result<std::shared_ptr<IConnection>>> connect(const Endpoint& endpoint) override;

    void close();

private:
    mutable std::mutex mutex_;
    std::string endpoint_;
    AcceptHandler accept_handler_;
    bool closed_{false};
    std::vector<std::weak_ptr<IConnection>> connections_;
};

using LoopbackTransport = LoopbackHub;

}  // namespace industrial_bus::transport
