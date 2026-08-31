#include "industrial_bus/bus_client.h"

#include "industrial_bus/client/default_executor.h"
#include "industrial_bus/client/pending_request_table.h"
#include "industrial_bus/client/runtime_adapter.h"
#include "industrial_bus/ids.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace industrial_bus {
namespace {

std::mutex g_client_mutex;
std::weak_ptr<BusClient> g_process_client;

std::int64_t unixDeadline(Duration timeout) {
    const auto deadline = std::chrono::system_clock::now() + timeout;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               deadline.time_since_epoch())
        .count();
}

struct SubscriptionEntry {
    std::mutex mutex;
    bool active{true};
    std::string payload_type;
    std::function<void(const ByteBuffer&)> handler;
    std::shared_ptr<IExecutor> executor;
};

class SubscriptionRegistry : public std::enable_shared_from_this<SubscriptionRegistry> {
public:
    Subscription add(std::string topic,
                     std::string payload_type,
                     std::function<void(const ByteBuffer&)> handler,
                     std::shared_ptr<IExecutor> executor) {
        auto entry = std::make_shared<SubscriptionEntry>();
        entry->payload_type = std::move(payload_type);
        entry->handler = std::move(handler);
        entry->executor = std::move(executor);

        std::uint64_t id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return {};
            }
            id = next_id_++;
            entries_[std::move(topic)].emplace(id, entry);
        }
        std::weak_ptr<SubscriptionRegistry> weak = shared_from_this();
        return Subscription([weak, id] {
            if (auto registry = weak.lock()) {
                registry->cancel(id);
            }
        });
    }

    void dispatch(const client::InboundMessage& message) {
        std::vector<std::shared_ptr<SubscriptionEntry>> matches;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto topic_it = entries_.find(message.topic);
            if (topic_it == entries_.end() || stopping_) {
                return;
            }
            for (const auto& item : topic_it->second) {
                matches.push_back(item.second);
            }
        }

        for (auto& entry : matches) {
            if (entry->payload_type != message.payload_type) {
                continue;
            }
            const auto payload = message.payload;
            entry->executor->post([entry, payload] {
                std::function<void(const ByteBuffer&)> handler;
                {
                    std::lock_guard<std::mutex> lock(entry->mutex);
                    if (!entry->active) {
                        return;
                    }
                    handler = entry->handler;
                }
                try {
                    handler(payload);
                } catch (...) {
                    // User callback failures must not escape into an executor.
                }
            });
        }
    }

    void cancelAll() noexcept {
        std::vector<std::shared_ptr<SubscriptionEntry>> entries;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            for (auto& by_topic : entries_) {
                for (auto& item : by_topic.second) {
                    entries.push_back(item.second);
                }
            }
            entries_.clear();
        }
        for (auto& entry : entries) {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->active = false;
            entry->handler = {};
        }
    }

public:
    // Kept explicit instead of making the registry part of the public API.
    static std::shared_ptr<SubscriptionRegistry> create() {
        struct SharedRegistry final : SubscriptionRegistry {};
        return std::make_shared<SharedRegistry>();
    }

private:
    void cancel(std::uint64_t id) noexcept {
        std::shared_ptr<SubscriptionEntry> removed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto topic_it = entries_.begin(); topic_it != entries_.end(); ++topic_it) {
                auto item = topic_it->second.find(id);
                if (item == topic_it->second.end()) {
                    continue;
                }
                removed = std::move(item->second);
                topic_it->second.erase(item);
                if (topic_it->second.empty()) {
                    entries_.erase(topic_it);
                }
                break;
            }
        }
        if (removed) {
            std::lock_guard<std::mutex> lock(removed->mutex);
            removed->active = false;
            removed->handler = {};
        }
    }

    std::mutex mutex_;
    std::unordered_map<std::string,
                       std::unordered_map<std::uint64_t, std::shared_ptr<SubscriptionEntry>>>
        entries_;
    std::uint64_t next_id_{1};
    bool stopping_{false};
};

using ServiceHandler =
    std::function<Result<ByteBuffer>(const ByteBuffer&, const MessageContext&)>;

struct ServiceHandlerEntry {
    std::mutex mutex;
    bool active{true};
    bool command_handler{false};
    std::string input_type;
    std::string output_type;
    ServiceHandler handler;
    std::shared_ptr<IExecutor> executor;
};

class ServiceHandlerRegistry
    : public std::enable_shared_from_this<ServiceHandlerRegistry> {
public:
    Subscription add(bool command_handler,
                     std::string topic,
                     std::string input_type,
                     std::string output_type,
                     ServiceHandler handler,
                     std::shared_ptr<IExecutor> executor) {
        auto entry = std::make_shared<ServiceHandlerEntry>();
        entry->command_handler = command_handler;
        entry->input_type = std::move(input_type);
        entry->output_type = std::move(output_type);
        entry->handler = std::move(handler);
        entry->executor = std::move(executor);

        const auto key = makeKey(command_handler, topic);
        std::uint64_t id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || entries_.count(key) != 0) {
                return {};
            }
            id = next_id_++;
            entries_.emplace(key, StoredEntry{id, entry});
        }
        std::weak_ptr<ServiceHandlerRegistry> weak = shared_from_this();
        return Subscription([weak, key, id] {
            if (auto registry = weak.lock()) {
                registry->cancel(key, id);
            }
        });
    }

    void dispatch(const client::InboundMessage& message,
                  const std::shared_ptr<client::IRuntimeAdapter>& adapter) {
        const bool is_command = message.kind == client::MessageKind::Command;
        std::shared_ptr<ServiceHandlerEntry> entry;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopping_) {
                const auto found = entries_.find(makeKey(is_command, message.topic));
                if (found != entries_.end()) {
                    entry = found->second.entry;
                }
            }
        }

        if (!entry) {
            sendError(message, adapter,
                      Error::serviceUnavailable("no handler registered for topic '" +
                                                message.topic + "'"));
            return;
        }
        if (entry->input_type != message.payload_type) {
            sendError(message, adapter,
                      Error::protocol("unexpected payload type for topic '" +
                                      message.topic + "'"));
            return;
        }

        entry->executor->post([entry, message, adapter] {
            ServiceHandler handler;
            std::string output_type;
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                if (!entry->active) {
                    return;
                }
                handler = entry->handler;
                output_type = entry->output_type;
            }

            MessageContext context;
            context.message_id = message.message_id;
            context.correlation_id = message.correlation_id;
            context.operation_id = message.operation_id;
            context.source_service = message.source_service;
            context.source_instance = message.source_instance;
            context.idempotency_key = message.idempotency_key;
            context.deadline_unix_ns = message.deadline_unix_ns;

            Result<ByteBuffer> result = Error::internal("handler did not complete");
            try {
                result = handler(message.payload, context);
            } catch (const std::exception& error) {
                result = Error::internal(std::string("service handler threw: ") +
                                         error.what());
            } catch (...) {
                result = Error::internal("service handler threw an unknown exception");
            }

            client::OutboundMessage reply = makeReply(message);
            if (result) {
                reply.payload_role = entry->command_handler
                                         ? client::PayloadRole::CommandAck
                                         : client::PayloadRole::ResponseData;
                reply.payload_type = std::move(output_type);
                reply.payload = std::move(result.value());
            } else {
                reply.payload_role = client::PayloadRole::ErrorDetail;
                reply.payload_type = "industrial_bus.system.v1.Error";
                reply.error = std::make_shared<Error>(std::move(result.error()));
            }
            (void)adapter->send(std::move(reply));
        });
    }

    void cancelAll() noexcept {
        std::vector<std::shared_ptr<ServiceHandlerEntry>> entries;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            for (auto& item : entries_) {
                entries.push_back(item.second.entry);
            }
            entries_.clear();
        }
        for (auto& entry : entries) {
            deactivate(entry);
        }
    }

    static std::shared_ptr<ServiceHandlerRegistry> create() {
        struct SharedRegistry final : ServiceHandlerRegistry {};
        return std::make_shared<SharedRegistry>();
    }

private:
    struct StoredEntry {
        std::uint64_t id;
        std::shared_ptr<ServiceHandlerEntry> entry;
    };

    static std::string makeKey(bool command_handler, const std::string& topic) {
        return std::string(command_handler ? "C:" : "R:") + topic;
    }

    static client::OutboundMessage makeReply(const client::InboundMessage& message) {
        client::OutboundMessage reply;
        reply.kind = client::MessageKind::Reply;
        reply.message_id = generateUuidV4();
        reply.correlation_id = message.correlation_id;
        reply.causation_id = message.message_id;
        reply.operation_id = message.operation_id;
        reply.destination = Destination{message.source_service, message.source_instance};
        reply.topic = message.topic;
        return reply;
    }

    static void sendError(const client::InboundMessage& message,
                          const std::shared_ptr<client::IRuntimeAdapter>& adapter,
                          Error error) {
        auto reply = makeReply(message);
        reply.payload_role = client::PayloadRole::ErrorDetail;
        reply.payload_type = "industrial_bus.system.v1.Error";
        reply.error = std::make_shared<Error>(std::move(error));
        (void)adapter->send(std::move(reply));
    }

    static void deactivate(const std::shared_ptr<ServiceHandlerEntry>& entry) noexcept {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->active = false;
        entry->handler = {};
    }

    void cancel(const std::string& key, std::uint64_t id) noexcept {
        std::shared_ptr<ServiceHandlerEntry> removed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end() && found->second.id == id) {
                removed = std::move(found->second.entry);
                entries_.erase(found);
            }
        }
        if (removed) {
            deactivate(removed);
        }
    }

    std::mutex mutex_;
    std::unordered_map<std::string, StoredEntry> entries_;
    std::uint64_t next_id_{1};
    bool stopping_{false};
};

struct CallbackState {
    std::shared_ptr<client::PendingRequestTable> pending;
    std::shared_ptr<SubscriptionRegistry> subscriptions;
    std::shared_ptr<ServiceHandlerRegistry> service_handlers;
    std::atomic<ConnectionState> connection{ConnectionState::Disconnected};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> accepting{true};
};

}  // namespace

class BusClient::Impl {
public:
    explicit Impl(const ClientOptions& options)
        : adapter(options.runtime_adapter),
          executor(std::make_shared<client::DefaultExecutor>(options.callback_threads)),
          state(std::make_shared<CallbackState>()) {
        state->pending = std::make_shared<client::PendingRequestTable>();
        state->subscriptions = SubscriptionRegistry::create();
        state->service_handlers = ServiceHandlerRegistry::create();
    }

    Result<void> start() {
        if (!adapter) {
            return Error::invalidArgument("ClientOptions.runtime_adapter is required");
        }
        std::weak_ptr<CallbackState> weak = state;
        adapter->setReceiveHandler([weak, adapter = adapter](client::InboundMessage message) {
            auto current = weak.lock();
            if (!current || !current->accepting.load(std::memory_order_acquire)) {
                return;
            }
            if (message.kind == client::MessageKind::Event ||
                message.kind == client::MessageKind::Operation) {
                current->subscriptions->dispatch(message);
                return;
            }
            if (message.kind == client::MessageKind::Command ||
                message.kind == client::MessageKind::Request) {
                current->service_handlers->dispatch(message, adapter);
                return;
            }
            if (message.kind != client::MessageKind::Reply) {
                return;
            }
            if (message.has_error) {
                current->pending->complete(message.causation_id, {}, message.error);
            } else {
                current->pending->complete(message.causation_id, message.payload_type,
                                           std::move(message.payload));
            }
        });
        adapter->setConnectionHandler(
            [weak](ConnectionState connection, std::uint64_t generation) {
                auto current = weak.lock();
                if (!current || !current->accepting.load(std::memory_order_acquire)) {
                    return;
                }
                const auto previous = current->generation.exchange(generation);
                current->connection.store(connection);
                if (connection == ConnectionState::Connected && previous != 0 &&
                    generation != previous) {
                    current->pending->failAll(
                        Error::hubRestarted("CommHub connection generation changed"));
                }
            });
        auto result = adapter->start();
        if (!result) {
            adapter->setReceiveHandler({});
            adapter->setConnectionHandler({});
            return result;
        }
        state->connection.store(adapter->state());
        state->generation.store(adapter->generation());
        return {};
    }

    std::shared_ptr<client::IRuntimeAdapter> adapter;
    std::shared_ptr<client::DefaultExecutor> executor;
    std::shared_ptr<CallbackState> state;
    std::mutex shutdown_mutex;
    bool shut_down{false};
};

Result<std::shared_ptr<BusClient>> BusClient::connect(const ClientOptions& options) {
    std::lock_guard<std::mutex> lock(g_client_mutex);
    if (auto existing = g_process_client.lock()) {
        const auto state = existing->connectionState();
        if (state != ConnectionState::Disconnected &&
            state != ConnectionState::ShuttingDown) {
            return existing;
        }
    }

    auto impl = std::make_unique<Impl>(options);
    auto result = impl->start();
    if (!result) {
        return result.error();
    }
    auto client = std::shared_ptr<BusClient>(new BusClient(std::move(impl)));
    g_process_client = client;
    return client;
}

BusClient::BusClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

BusClient::~BusClient() {
    if (impl_) {
        shutdown(std::chrono::milliseconds(0));
    }
}

Result<void> BusClient::publishRaw(std::string topic,
                                   std::string payload_type,
                                   ByteBuffer payload,
                                   PublishOptions options) {
    if (topic.empty() || payload_type.empty()) {
        return Error::invalidArgument("event topic and payload type must not be empty");
    }
    if (!impl_->state->accepting.load() ||
        impl_->state->connection.load() != ConnectionState::Connected) {
        return Error::connectionLost("client is not connected");
    }
    client::OutboundMessage message;
    message.kind = client::MessageKind::Event;
    message.payload_role = client::PayloadRole::EventData;
    message.message_id = generateUuidV4();
    message.correlation_id = options.correlation_id.empty() ? generateUuidV4()
                                                              : std::move(options.correlation_id);
    message.causation_id = std::move(options.causation_id);
    message.operation_id = std::move(options.operation_id);
    message.topic = std::move(topic);
    message.payload_type = std::move(payload_type);
    message.payload = std::move(payload);
    return impl_->adapter->send(std::move(message));
}

Subscription BusClient::subscribeRaw(std::string topic,
                                     std::string payload_type,
                                     RawEventHandler handler,
                                     std::shared_ptr<IExecutor> executor) {
    if (topic.empty() || payload_type.empty() || !handler ||
        !impl_->state->accepting.load()) {
        return {};
    }
    if (!executor) {
        executor = impl_->executor;
    }
    const auto subscription_result = impl_->adapter->subscribe(topic);
    if (!subscription_result) {
        return {};
    }
    return impl_->state->subscriptions->add(std::move(topic), std::move(payload_type),
                                             std::move(handler), std::move(executor));
}

Subscription BusClient::registerHandlerRaw(bool command_handler,
                                           std::string topic,
                                           std::string input_type,
                                           std::string output_type,
                                           RawServiceHandler handler,
                                           std::shared_ptr<IExecutor> executor) {
    if (topic.empty() || input_type.empty() || output_type.empty() || !handler ||
        !impl_->state->accepting.load()) {
        return {};
    }
    if (!executor) {
        executor = impl_->executor;
    }
    return impl_->state->service_handlers->add(
        command_handler, std::move(topic), std::move(input_type),
        std::move(output_type), std::move(handler), std::move(executor));
}

void BusClient::commandRaw(Destination destination,
                           std::string topic,
                           std::string command_type,
                           std::string ack_type,
                           ByteBuffer payload,
                           CommandOptions options,
                           RawCompletion completion) {
    if (destination.empty() || topic.empty() || command_type.empty() || ack_type.empty() ||
        options.idempotency_key.empty() || options.accept_timeout <= Duration::zero()) {
        completion(Error::invalidArgument(
            "command requires destination, topic, payload types, idempotency key and timeout"));
        return;
    }
    if (!impl_->state->accepting.load() ||
        impl_->state->connection.load() != ConnectionState::Connected) {
        completion(Error::connectionLost("client is not connected"));
        return;
    }

    client::OutboundMessage message;
    message.kind = client::MessageKind::Command;
    message.payload_role = client::PayloadRole::CommandData;
    message.message_id = generateUuidV4();
    message.correlation_id = options.correlation_id.empty() ? generateUuidV4()
                                                              : std::move(options.correlation_id);
    message.operation_id = std::move(options.operation_id);
    message.destination = std::move(destination);
    message.topic = std::move(topic);
    message.payload_type = std::move(command_type);
    message.payload = std::move(payload);
    message.idempotency_key = std::move(options.idempotency_key);
    message.deadline_unix_ns = unixDeadline(options.accept_timeout);

    const auto message_id = message.message_id;
    const auto deadline = client::PendingRequestTable::Clock::now() + options.accept_timeout;
    if (!impl_->state->pending->add(message_id, std::move(ack_type), deadline,
                                    completion)) {
        completion(Error::resourceExhausted("cannot register pending command"));
        return;
    }
    auto send_result = impl_->adapter->send(std::move(message));
    if (!send_result) {
        impl_->state->pending->complete(message_id, {}, send_result.error());
    }
}

void BusClient::requestRaw(Destination destination,
                           std::string topic,
                           std::string request_type,
                           std::string response_type,
                           ByteBuffer payload,
                           Duration timeout,
                           RawCompletion completion) {
    if (destination.empty() || topic.empty() || request_type.empty() ||
        response_type.empty() || timeout <= Duration::zero()) {
        completion(Error::invalidArgument(
            "request requires destination, topic, payload types and positive timeout"));
        return;
    }
    if (!impl_->state->accepting.load() ||
        impl_->state->connection.load() != ConnectionState::Connected) {
        completion(Error::connectionLost("client is not connected"));
        return;
    }

    client::OutboundMessage message;
    message.kind = client::MessageKind::Request;
    message.payload_role = client::PayloadRole::RequestData;
    message.message_id = generateUuidV4();
    message.correlation_id = generateUuidV4();
    message.destination = std::move(destination);
    message.topic = std::move(topic);
    message.payload_type = std::move(request_type);
    message.payload = std::move(payload);
    message.deadline_unix_ns = unixDeadline(timeout);

    const auto message_id = message.message_id;
    const auto deadline = client::PendingRequestTable::Clock::now() + timeout;
    if (!impl_->state->pending->add(message_id, std::move(response_type), deadline,
                                    completion)) {
        completion(Error::resourceExhausted("cannot register pending request"));
        return;
    }
    auto send_result = impl_->adapter->send(std::move(message));
    if (!send_result) {
        impl_->state->pending->complete(message_id, {}, send_result.error());
    }
}

Result<void> BusClient::shutdown(Duration timeout) {
    std::lock_guard<std::mutex> lock(impl_->shutdown_mutex);
    if (impl_->shut_down) {
        return {};
    }
    impl_->shut_down = true;
    impl_->state->accepting.store(false);
    impl_->state->connection.store(ConnectionState::ShuttingDown);
    impl_->adapter->setReceiveHandler({});
    impl_->adapter->setConnectionHandler({});
    impl_->state->subscriptions->cancelAll();
    impl_->state->service_handlers->cancelAll();
    impl_->state->pending->shutdown(
        Error::make(ErrorCategory::Cancelled, "client runtime shut down"));
    const bool flushed = impl_->adapter->flush(timeout);
    impl_->adapter->close();
    impl_->executor->shutdown();
    impl_->state->connection.store(ConnectionState::Disconnected);
    if (!flushed && timeout > Duration::zero()) {
        return Error::timeout("client shutdown flush timed out");
    }
    return {};
}

ConnectionState BusClient::connectionState() const {
    return impl_->state->connection.load();
}

ServiceSnapshot BusClient::services() const {
    return impl_->adapter ? impl_->adapter->services() : ServiceSnapshot{};
}

Result<void> BusClient::reportOperation(OperationUpdate update,
                                        std::string correlation_id) {
    if (update.operation_id.empty() || update.operation_type.empty() ||
        update.state == OperationState::Unspecified) {
        return Error::invalidArgument(
            "operation update requires operation id, type and state");
    }
    if (!impl_->state->accepting.load() ||
        impl_->state->connection.load() != ConnectionState::Connected) {
        return Error::connectionLost("client is not connected");
    }
    client::OutboundMessage message;
    message.kind = client::MessageKind::Operation;
    message.payload_role = client::PayloadRole::OperationData;
    message.message_id = generateUuidV4();
    message.correlation_id = correlation_id.empty() ? generateUuidV4()
                                                     : std::move(correlation_id);
    message.operation_id = update.operation_id;
    message.topic = "system.operation.event";
    message.payload_type = "industrial_bus.system.v1.OperationEvent";
    message.operation = std::make_shared<OperationUpdate>(std::move(update));
    return impl_->adapter->send(std::move(message));
}

}  // namespace industrial_bus
