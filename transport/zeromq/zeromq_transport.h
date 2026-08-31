#pragma once

#include "industrial_bus/transport/transport.h"

#include <cstddef>
#include <memory>

namespace industrial_bus::transport {

struct ZeroMqOptions {
    std::size_t outbound_capacity{1024};
    int io_threads{1};
};

class ZeroMqDealerConnector final : public IConnector {
public:
    explicit ZeroMqDealerConnector(ZeroMqOptions options = {});
    ~ZeroMqDealerConnector() override;

    Future<Result<std::shared_ptr<IConnection>>> connect(const Endpoint& endpoint) override;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

class ZeroMqRouterListener final : public IListener {
public:
    explicit ZeroMqRouterListener(ZeroMqOptions options = {});
    ~ZeroMqRouterListener() override;

    Result<void> listen(const Endpoint& endpoint, AcceptHandler handler) override;
    void close();

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace industrial_bus::transport
