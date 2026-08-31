#include "zeromq_transport.h"

#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace industrial_bus::transport {
namespace {

using namespace std::chrono_literals;

class DealerConnection final : public IConnection {
public:
    DealerConnection(std::shared_ptr<zmq::context_t> context,
                     std::string endpoint,
                     std::size_t capacity)
        : context_(std::move(context)), endpoint_(std::move(endpoint)), capacity_(capacity) {}

    ~DealerConnection() override { close(); }

    Result<void> start() {
        worker_ = std::thread([this] { run(); });
        std::unique_lock<std::mutex> lock(start_mutex_);
        if (!start_cv_.wait_for(lock, 5s, [this] { return started_; })) {
            return Result<void>::failure({ErrorCode::transport_failure,
                                          "timed out starting ZeroMQ DEALER"});
        }
        if (state_.load() != ConnectionState::connected) {
            return Result<void>::failure({ErrorCode::transport_failure, start_error_});
        }
        return Result<void>::success();
    }

    Result<void> send(ByteView frame) override {
        if (state_.load() != ConnectionState::connected) {
            return Result<void>::failure({ErrorCode::not_connected,
                                          "ZeroMQ DEALER is not connected"});
        }
        ByteBuffer copy;
        if (!frame.empty()) {
            copy.assign(frame.data(), frame.data() + frame.size());
        }
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        if (outbound_.size() >= capacity_) {
            return Result<void>::failure({ErrorCode::resource_exhausted,
                                          "ZeroMQ DEALER outbound queue is full"});
        }
        outbound_.push_back(std::move(copy));
        return Result<void>::success();
    }

    void setReceiveHandler(ReceiveHandler handler) override {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler_ = std::move(handler);
    }

    ConnectionState state() const override { return state_.load(); }

    void close() override {
        const auto old = state_.exchange(ConnectionState::closing);
        if (old == ConnectionState::closed || old == ConnectionState::closing) {
            return;
        }
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        state_.store(ConnectionState::closed);
    }

private:
    void signalStarted(std::string error = {}) {
        {
            std::lock_guard<std::mutex> lock(start_mutex_);
            start_error_ = std::move(error);
            started_ = true;
        }
        start_cv_.notify_all();
    }

    void run() {
        try {
            zmq::socket_t socket(*context_, zmq::socket_type::dealer);
            socket.set(zmq::sockopt::linger, 0);
            socket.connect(endpoint_);
            state_.store(ConnectionState::connected);
            signalStarted();

            while (!stop_.load()) {
                ByteBuffer outbound;
                bool has_outbound = false;
                {
                    std::lock_guard<std::mutex> lock(outbound_mutex_);
                    if (!outbound_.empty()) {
                        outbound = std::move(outbound_.front());
                        outbound_.pop_front();
                        has_outbound = true;
                    }
                }
                if (has_outbound) {
                    const auto sent = socket.send(zmq::buffer(outbound), zmq::send_flags::dontwait);
                    if (!sent) {
                        std::lock_guard<std::mutex> lock(outbound_mutex_);
                        outbound_.push_front(std::move(outbound));
                    }
                }

                zmq::message_t incoming;
                const auto received = socket.recv(incoming, zmq::recv_flags::dontwait);
                if (received) {
                    ByteBuffer frame(static_cast<const std::uint8_t*>(incoming.data()),
                                     static_cast<const std::uint8_t*>(incoming.data()) + incoming.size());
                    ReceiveHandler handler;
                    {
                        std::lock_guard<std::mutex> lock(handler_mutex_);
                        handler = handler_;
                    }
                    if (handler) {
                        handler(std::move(frame));
                    }
                } else if (!has_outbound) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            socket.close();
        } catch (const zmq::error_t& error) {
            state_.store(ConnectionState::failed);
            signalStarted(error.what());
        }
    }

    std::shared_ptr<zmq::context_t> context_;
    std::string endpoint_;
    std::size_t capacity_;
    std::atomic<ConnectionState> state_{ConnectionState::connecting};
    std::atomic<bool> stop_{false};
    std::thread worker_;

    std::mutex outbound_mutex_;
    std::deque<ByteBuffer> outbound_;
    std::mutex handler_mutex_;
    ReceiveHandler handler_;

    std::mutex start_mutex_;
    std::condition_variable start_cv_;
    bool started_{false};
    std::string start_error_;
};

struct RouterOutbound {
    std::string identity;
    ByteBuffer frame;
};

struct RouterState {
    explicit RouterState(std::shared_ptr<zmq::context_t> context, std::size_t capacity)
        : context(std::move(context)), capacity(capacity) {}

    Result<void> enqueue(std::string identity, ByteView frame) {
        ByteBuffer copy;
        if (!frame.empty()) {
            copy.assign(frame.data(), frame.data() + frame.size());
        }
        std::lock_guard<std::mutex> lock(outbound_mutex);
        if (outbound.size() >= capacity) {
            return Result<void>::failure({ErrorCode::resource_exhausted,
                                          "ZeroMQ ROUTER outbound queue is full"});
        }
        outbound.push_back({std::move(identity), std::move(copy)});
        return Result<void>::success();
    }

    std::shared_ptr<zmq::context_t> context;
    std::size_t capacity;
    std::atomic<bool> stop{false};
    std::atomic<ConnectionState> state{ConnectionState::disconnected};
    std::mutex outbound_mutex;
    std::deque<RouterOutbound> outbound;
};

class RouterPeer final : public IConnection {
public:
    RouterPeer(std::weak_ptr<RouterState> state, std::string identity)
        : state_(std::move(state)), identity_(std::move(identity)) {}

    Result<void> send(ByteView frame) override {
        auto state = state_.lock();
        if (!state || closed_.load() || state->state.load() != ConnectionState::connected) {
            return Result<void>::failure({ErrorCode::not_connected,
                                          "ZeroMQ ROUTER peer is not connected"});
        }
        return state->enqueue(identity_, frame);
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
        auto state = state_.lock();
        return !closed_.load() && state ? state->state.load() : ConnectionState::closed;
    }

    void close() override { closed_.store(true); }

    void deliver(ByteBuffer frame) {
        ReceiveHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_.load()) {
                return;
            }
            handler = handler_;
            if (!handler) {
                pending_.push_back(std::move(frame));
                return;
            }
        }
        handler(std::move(frame));
    }

private:
    std::weak_ptr<RouterState> state_;
    std::string identity_;
    std::atomic<bool> closed_{false};
    mutable std::mutex mutex_;
    ReceiveHandler handler_;
    std::deque<ByteBuffer> pending_;
};

}  // namespace

class ZeroMqDealerConnector::Impl {
public:
    explicit Impl(ZeroMqOptions options)
        : options(std::move(options)), context(std::make_shared<zmq::context_t>(options.io_threads)) {}

    ZeroMqOptions options;
    std::shared_ptr<zmq::context_t> context;
};

ZeroMqDealerConnector::ZeroMqDealerConnector(ZeroMqOptions options)
    : impl_(std::make_shared<Impl>(std::move(options))) {}

ZeroMqDealerConnector::~ZeroMqDealerConnector() = default;

Future<Result<std::shared_ptr<IConnection>>> ZeroMqDealerConnector::connect(
    const Endpoint& endpoint) {
    if (endpoint.empty()) {
        std::promise<Result<std::shared_ptr<IConnection>>> promise;
        promise.set_value(Result<std::shared_ptr<IConnection>>::failure(
            {ErrorCode::invalid_argument, "ZeroMQ endpoint is required"}));
        return promise.get_future();
    }
    auto impl = impl_;
    return std::async(std::launch::async, [impl, endpoint] {
        auto connection = std::make_shared<DealerConnection>(
            impl->context, endpoint.value, impl->options.outbound_capacity);
        const auto started = connection->start();
        if (!started) {
            connection->close();
            return Result<std::shared_ptr<IConnection>>::failure(started.error());
        }
        return Result<std::shared_ptr<IConnection>>::success(connection);
    });
}

class ZeroMqRouterListener::Impl : public std::enable_shared_from_this<Impl> {
public:
    explicit Impl(ZeroMqOptions options)
        : options(std::move(options)),
          state(std::make_shared<RouterState>(
              std::make_shared<zmq::context_t>(this->options.io_threads),
              this->options.outbound_capacity)) {}

    Result<void> listen(const Endpoint& endpoint, AcceptHandler handler) {
        if (endpoint.empty() || !handler) {
            return Result<void>::failure({ErrorCode::invalid_argument,
                                          "ZeroMQ endpoint and accept handler are required"});
        }
        if (worker.joinable()) {
            return Result<void>::failure({ErrorCode::already_listening,
                                          "ZeroMQ ROUTER already listens"});
        }
        std::promise<Result<void>> startup;
        auto result = startup.get_future();
        worker = std::thread([self = shared_from_this(), endpoint, handler = std::move(handler),
                              startup = std::move(startup)]() mutable {
            self->run(endpoint.value, std::move(handler), std::move(startup));
        });
        return result.get();
    }

    void close() {
        state->stop.store(true);
        if (worker.joinable()) {
            worker.join();
        }
        state->state.store(ConnectionState::closed);
        std::lock_guard<std::mutex> lock(peers_mutex);
        for (auto& entry : peers) {
            entry.second->close();
        }
        peers.clear();
    }

private:
    void run(std::string endpoint, AcceptHandler accept, std::promise<Result<void>> startup) {
        try {
            zmq::socket_t socket(*state->context, zmq::socket_type::router);
            socket.set(zmq::sockopt::linger, 0);
            socket.set(zmq::sockopt::sndtimeo, 10);
            socket.bind(endpoint);
            state->state.store(ConnectionState::connected);
            startup.set_value(Result<void>::success());

            while (!state->stop.load()) {
                RouterOutbound outbound;
                bool has_outbound = false;
                {
                    std::lock_guard<std::mutex> lock(state->outbound_mutex);
                    if (!state->outbound.empty()) {
                        outbound = std::move(state->outbound.front());
                        state->outbound.pop_front();
                        has_outbound = true;
                    }
                }
                if (has_outbound) {
                    const auto identity_sent = socket.send(
                        zmq::buffer(outbound.identity), zmq::send_flags::sndmore | zmq::send_flags::dontwait);
                    const auto frame_sent = identity_sent
                        ? socket.send(zmq::buffer(outbound.frame), zmq::send_flags::none)
                        : zmq::send_result_t{};
                    if (!identity_sent || !frame_sent) {
                        std::lock_guard<std::mutex> lock(state->outbound_mutex);
                        state->outbound.push_front(std::move(outbound));
                    }
                }

                zmq::message_t identity_message;
                const auto received = socket.recv(identity_message, zmq::recv_flags::dontwait);
                if (!received) {
                    if (!has_outbound) {
                        std::this_thread::sleep_for(1ms);
                    }
                    continue;
                }
                const std::string identity(static_cast<const char*>(identity_message.data()),
                                           identity_message.size());
                zmq::message_t frame_message;
                if (!socket.recv(frame_message, zmq::recv_flags::none)) {
                    continue;
                }
                while (frame_message.more()) {
                    zmq::message_t next;
                    if (!socket.recv(next, zmq::recv_flags::none)) {
                        break;
                    }
                    frame_message = std::move(next);
                }

                std::shared_ptr<RouterPeer> peer;
                bool is_new = false;
                {
                    std::lock_guard<std::mutex> lock(peers_mutex);
                    auto found = peers.find(identity);
                    if (found == peers.end()) {
                        peer = std::make_shared<RouterPeer>(state, identity);
                        peers.emplace(identity, peer);
                        is_new = true;
                    } else {
                        peer = found->second;
                    }
                }
                if (is_new) {
                    accept(peer);
                }
                ByteBuffer frame(static_cast<const std::uint8_t*>(frame_message.data()),
                                 static_cast<const std::uint8_t*>(frame_message.data()) +
                                     frame_message.size());
                peer->deliver(std::move(frame));
            }
            socket.close();
        } catch (const zmq::error_t& error) {
            state->state.store(ConnectionState::failed);
            try {
                startup.set_value(Result<void>::failure(
                    {ErrorCode::transport_failure, error.what()}));
            } catch (const std::future_error&) {
                // Startup was already reported; the state exposes the runtime failure.
            }
        }
    }

    ZeroMqOptions options;
    std::shared_ptr<RouterState> state;
    std::thread worker;
    std::mutex peers_mutex;
    std::unordered_map<std::string, std::shared_ptr<RouterPeer>> peers;
};

ZeroMqRouterListener::ZeroMqRouterListener(ZeroMqOptions options)
    : impl_(std::make_shared<Impl>(std::move(options))) {}

ZeroMqRouterListener::~ZeroMqRouterListener() { close(); }

Result<void> ZeroMqRouterListener::listen(const Endpoint& endpoint, AcceptHandler handler) {
    return impl_->listen(endpoint, std::move(handler));
}

void ZeroMqRouterListener::close() {
    if (impl_) {
        impl_->close();
    }
}

}  // namespace industrial_bus::transport
