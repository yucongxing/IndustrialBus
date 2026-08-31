#include "loopback_transport.h"

#include <deque>
#include <mutex>

namespace industrial_bus::transport {
namespace {

class LoopbackConnection final : public IConnection,
                                 public std::enable_shared_from_this<LoopbackConnection> {
public:
    Result<void> send(ByteView frame) override {
        std::shared_ptr<LoopbackConnection> peer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != ConnectionState::connected) {
                return Result<void>::failure({ErrorCode::not_connected,
                                              "loopback connection is not connected"});
            }
            peer = peer_.lock();
        }
        if (!peer) {
            return Result<void>::failure({ErrorCode::not_connected,
                                          "loopback peer is no longer available"});
        }
        ByteBuffer copy;
        if (!frame.empty()) {
            copy.assign(frame.data(), frame.data() + frame.size());
        }
        return peer->deliver(std::move(copy));
    }

    void setReceiveHandler(ReceiveHandler handler) override {
        std::deque<ByteBuffer> pending;
        ReceiveHandler installed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler_ = std::move(handler);
            installed = handler_;
            if (installed) {
                pending.swap(pending_);
            }
        }
        for (auto& frame : pending) {
            installed(std::move(frame));
        }
    }

    ConnectionState state() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    void close() override {
        std::shared_ptr<LoopbackConnection> peer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == ConnectionState::closed) {
                return;
            }
            state_ = ConnectionState::closed;
            pending_.clear();
            peer = peer_.lock();
            peer_.reset();
        }
        if (peer) {
            peer->peerClosed();
        }
    }

    void attach(const std::shared_ptr<LoopbackConnection>& peer) {
        std::lock_guard<std::mutex> lock(mutex_);
        peer_ = peer;
        state_ = ConnectionState::connected;
    }

private:
    Result<void> deliver(ByteBuffer frame) {
        ReceiveHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != ConnectionState::connected) {
                return Result<void>::failure({ErrorCode::not_connected,
                                              "loopback destination is not connected"});
            }
            handler = handler_;
            if (!handler) {
                pending_.push_back(std::move(frame));
                return Result<void>::success();
            }
        }
        handler(std::move(frame));
        return Result<void>::success();
    }

    void peerClosed() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ConnectionState::connected) {
            state_ = ConnectionState::disconnected;
        }
        peer_.reset();
    }

    mutable std::mutex mutex_;
    std::weak_ptr<LoopbackConnection> peer_;
    ReceiveHandler handler_;
    std::deque<ByteBuffer> pending_;
    ConnectionState state_{ConnectionState::connecting};
};

template <typename T>
Future<T> readyFuture(T value) {
    std::promise<T> promise;
    promise.set_value(std::move(value));
    return promise.get_future();
}

}  // namespace

LoopbackHub::~LoopbackHub() { close(); }

Result<void> LoopbackHub::listen(const Endpoint& endpoint, AcceptHandler handler) {
    if (endpoint.empty() || !handler) {
        return Result<void>::failure({ErrorCode::invalid_argument,
                                      "loopback endpoint and accept handler are required"});
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return Result<void>::failure({ErrorCode::closed, "loopback hub is closed"});
    }
    if (accept_handler_) {
        return Result<void>::failure({ErrorCode::already_listening,
                                      "loopback hub already has a listener"});
    }
    endpoint_ = endpoint.value;
    accept_handler_ = std::move(handler);
    return Result<void>::success();
}

Future<Result<std::shared_ptr<IConnection>>> LoopbackHub::connect(const Endpoint& endpoint) {
    AcceptHandler accept;
    std::shared_ptr<LoopbackConnection> client;
    std::shared_ptr<LoopbackConnection> server;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return readyFuture(Result<std::shared_ptr<IConnection>>::failure(
                {ErrorCode::closed, "loopback hub is closed"}));
        }
        if (!accept_handler_ || endpoint.value != endpoint_) {
            return readyFuture(Result<std::shared_ptr<IConnection>>::failure(
                {ErrorCode::endpoint_unavailable, "no listener for loopback endpoint"}));
        }
        client = std::make_shared<LoopbackConnection>();
        server = std::make_shared<LoopbackConnection>();
        client->attach(server);
        server->attach(client);
        connections_.push_back(client);
        connections_.push_back(server);
        accept = accept_handler_;
    }
    accept(server);
    return readyFuture(Result<std::shared_ptr<IConnection>>::success(client));
}

void LoopbackHub::close() {
    std::vector<std::shared_ptr<IConnection>> connections;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        accept_handler_ = {};
        for (auto& weak : connections_) {
            if (auto connection = weak.lock()) {
                connections.push_back(std::move(connection));
            }
        }
        connections_.clear();
    }
    for (auto& connection : connections) {
        connection->close();
    }
}

}  // namespace industrial_bus::transport
