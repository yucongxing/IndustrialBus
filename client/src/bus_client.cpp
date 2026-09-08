#include "industrial_bus/bus_client.h"

#include "industrial_bus/client/default_executor.h"
#include "industrial_bus/client/pending_request_table.h"
#include "industrial_bus/client/runtime_adapter.h"
#include "industrial_bus/core/idempotency_journal.h"
#include "industrial_bus/ids.h"

#include "operation.pb.h"
#include "system.pb.h"

#if defined(INDUSTRIAL_BUS_HAS_LINUX_RUNTIME)
#include "linux_runtime_adapter.h"
#endif

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

std::int64_t unixNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void applySemantics(client::OutboundMessage& message,
                    MessageClass message_class,
                    std::optional<MessageType> message_type = std::nullopt) {
    message.message_class = message_class;
    message.message_type = message_type.value_or(messageTypeFor(message_class));
    message.priority = defaultPriority(message_class);
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
    explicit SubscriptionRegistry(
        std::shared_ptr<client::IRuntimeAdapter> adapter)
        : adapter_(std::move(adapter)) {}

    Subscription add(std::string topic,
                     std::string payload_type,
                     std::function<void(const ByteBuffer&)> handler,
                     std::shared_ptr<IExecutor> executor) {
        auto entry = std::make_shared<SubscriptionEntry>();
        entry->payload_type = std::move(payload_type);
        entry->handler = std::move(handler);
        entry->executor = std::move(executor);

        const auto key = topic;
        std::uint64_t id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return {};
            }
            auto& by_id = entries_[key];
            if (by_id.empty()) {
                const auto subscribed = adapter_->subscribe(key);
                if (!subscribed) {
                    entries_.erase(key);
                    return {};
                }
            }
            id = next_id_++;
            by_id.emplace(id, entry);
        }
        std::weak_ptr<SubscriptionRegistry> weak = shared_from_this();
        return Subscription([weak, key, id] {
            if (auto registry = weak.lock()) {
                registry->cancel(key, id);
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
                (void)adapter_->unsubscribe(by_topic.first);
            }
            entries_.clear();
        }
        for (auto& entry : entries) {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->active = false;
            entry->handler = {};
        }
    }

    void resubscribeAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        for (const auto& by_topic : entries_) {
            (void)adapter_->subscribe(by_topic.first);
        }
    }

public:
    // Kept explicit instead of making the registry part of the public API.
    static std::shared_ptr<SubscriptionRegistry> create(
        std::shared_ptr<client::IRuntimeAdapter> adapter) {
        return std::make_shared<SubscriptionRegistry>(std::move(adapter));
    }

private:
    void cancel(const std::string& topic, std::uint64_t id) noexcept {
        std::shared_ptr<SubscriptionEntry> removed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto topic_it = entries_.find(topic);
            if (topic_it == entries_.end()) {
                return;
            }
            const auto item = topic_it->second.find(id);
            if (item == topic_it->second.end()) {
                return;
            }
            removed = std::move(item->second);
            topic_it->second.erase(item);
            if (topic_it->second.empty()) {
                (void)adapter_->unsubscribe(topic_it->first);
                entries_.erase(topic_it);
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
    std::shared_ptr<client::IRuntimeAdapter> adapter_;
    std::uint64_t next_id_{1};
    bool stopping_{false};
};

using ServiceHandler =
    std::function<Result<ByteBuffer>(const ByteBuffer&, const MessageContext&)>;
using PayloadValidator = std::function<Result<void>(const ByteBuffer&)>;

struct ServiceHandlerEntry {
    std::mutex mutex;
    bool active{true};
    bool command_handler{false};
    std::string input_type;
    std::string output_type;
    PayloadValidator validator;
    ServiceHandler handler;
    std::shared_ptr<IExecutor> executor;
};

class ServiceHandlerRegistry
    : public std::enable_shared_from_this<ServiceHandlerRegistry> {
public:
    ServiceHandlerRegistry(std::shared_ptr<core::IdempotencyJournal> journal,
                           ServiceInfo local_service)
        : journal_(std::move(journal)), local_service_(std::move(local_service)) {}

    Subscription add(bool command_handler,
                     std::string topic,
                     std::string input_type,
                     std::string output_type,
                     PayloadValidator validator,
                     ServiceHandler handler,
                     std::shared_ptr<IExecutor> executor) {
        auto entry = std::make_shared<ServiceHandlerEntry>();
        entry->command_handler = command_handler;
        entry->input_type = std::move(input_type);
        entry->output_type = std::move(output_type);
        entry->validator = std::move(validator);
        entry->handler = std::move(handler);
        entry->executor = std::move(executor);

        const auto key = makeKey(command_handler, topic);
        std::uint64_t id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || entries_.count(key) != 0 ||
                (command_handler &&
                 (!journal_ || local_service_.service_name.empty() ||
                  local_service_.instance_id.empty()))) {
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
        const bool is_command = message.message_type == MessageType::Command;
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
        if (entry->command_handler && entry->validator) {
            Result<void> validated = Error::protocol("payload validator did not complete");
            try {
                validated = entry->validator(message.payload);
            } catch (const std::exception& error) {
                validated = Error::protocol(std::string("payload validator threw: ") +
                                            error.what());
            } catch (...) {
                validated = Error::protocol("payload validator threw an unknown exception");
            }
            if (!validated) {
                sendError(message, adapter, validated.error());
                return;
            }
        }

        auto self = shared_from_this();
        entry->executor->post([self, entry, message, adapter] {
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

            if (entry->command_handler) {
                self->executeCommand(message, std::move(context),
                                     std::move(handler), adapter);
                return;
            }

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
                reply.payload_role = client::PayloadRole::ResponseData;
                reply.payload_type = std::move(output_type);
                reply.payload = std::move(result.value());
            } else {
                applySemantics(reply, MessageClass::Error, MessageType::Reply);
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

    static std::shared_ptr<ServiceHandlerRegistry> create(
        std::shared_ptr<core::IdempotencyJournal> journal,
        ServiceInfo local_service) {
        return std::make_shared<ServiceHandlerRegistry>(std::move(journal),
                                                        std::move(local_service));
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
        applySemantics(reply, MessageClass::Reply);
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
        applySemantics(reply, MessageClass::Error, MessageType::Reply);
        if (!error.operation_id.empty()) {
            reply.operation_id = error.operation_id;
        }
        reply.payload_role = client::PayloadRole::ErrorDetail;
        reply.payload_type = "industrial_bus.system.v1.Error";
        reply.error = std::make_shared<Error>(std::move(error));
        (void)adapter->send(std::move(reply));
    }

    static Result<ByteBuffer> encodeCommandAck(const core::IdempotencyRecord& record,
                                               bool duplicate) {
        system::v1::CommandAck ack;
        ack.set_operation_id(record.operation_id);
        ack.set_owner_service(record.owner_service);
        ack.set_owner_instance(record.owner_instance);
        ack.set_duplicate(duplicate);
        ack.set_current_state(
            static_cast<operation::v1::OperationState>(record.state));
        ByteBuffer payload;
        if (!PayloadCodec<system::v1::CommandAck>::encode(ack, payload)) {
            return Error::protocol("failed to serialize standard CommandAck");
        }
        return payload;
    }

    void sendCommandAck(const client::InboundMessage& message,
                        const core::IdempotencyRecord& record,
                        bool duplicate,
                        const std::shared_ptr<client::IRuntimeAdapter>& adapter) {
        auto encoded = encodeCommandAck(record, duplicate);
        if (!encoded) {
            sendError(message, adapter, encoded.error());
            return;
        }
        auto reply = makeReply(message);
        applySemantics(reply, MessageClass::Accepted);
        reply.operation_id = record.operation_id;
        reply.payload_role = client::PayloadRole::CommandAck;
        reply.payload_type = "industrial_bus.system.v1.CommandAck";
        reply.payload = std::move(encoded.value());
        (void)adapter->send(std::move(reply));
    }

    void executeCommand(const client::InboundMessage& message,
                        MessageContext context,
                        ServiceHandler handler,
                        const std::shared_ptr<client::IRuntimeAdapter>& adapter) {
        if (!journal_) {
            sendError(message, adapter,
                      Error::serviceUnavailable(
                          "Command handler requires a durable idempotency journal"));
            return;
        }
        if (local_service_.service_name.empty() || local_service_.instance_id.empty()) {
            sendError(message, adapter,
                      Error::internal("runtime adapter did not expose its local identity"));
            return;
        }
        if (message.source_service.empty() || message.idempotency_key.empty()) {
            sendError(message, adapter,
                      Error::protocol(
                          "Command requires source_service and idempotency_key"));
            return;
        }
        if (message.deadline_unix_ns != 0 && message.deadline_unix_ns <= unixNowNs()) {
            sendError(message, adapter,
                      Error::timeout("Command deadline elapsed before acceptance"));
            return;
        }

        core::CommandRegistration registration;
        registration.identity = {message.source_service, message.topic,
                                 message.idempotency_key};
        registration.payload = message.payload;
        registration.operation_id = message.operation_id.empty() ? generateUuidV4()
                                                                  : message.operation_id;
        registration.owner_service = local_service_.service_name;
        registration.owner_instance = local_service_.instance_id;
        auto decision_result = journal_->registerCommand(std::move(registration));
        if (!decision_result) {
            sendError(message, adapter, decision_result.error());
            return;
        }
        auto decision = std::move(decision_result.value());
        auto& record = decision.record;
        context.operation_id = record.operation_id;

        if (decision.duplicate && record.state == OperationState::Failed &&
            record.error) {
            sendError(message, adapter, *record.error);
            return;
        }
        if (decision.duplicate && !isTerminal(record.state) &&
            (record.owner_service != local_service_.service_name ||
             record.owner_instance != local_service_.instance_id)) {
            auto error = Error::ownerUnavailable(
                "Command operation belongs to an unavailable service instance");
            error.operation_id = record.operation_id;
            sendError(message, adapter, std::move(error));
            return;
        }
        if (decision.duplicate && record.state != OperationState::Created) {
            sendCommandAck(message, record, true, adapter);
            return;
        }

        auto claimed = journal_->claimForExecution(record.identity,
                                                   record.operation_id);
        if (!claimed) {
            sendError(message, adapter, claimed.error());
            return;
        }
        if (!claimed.value()) {
            auto current = journal_->find(record.identity);
            if (!current) {
                sendError(message, adapter, current.error());
                return;
            }
            sendCommandAck(message, current.value(), decision.duplicate, adapter);
            return;
        }
        record.state = OperationState::Running;

        Result<ByteBuffer> handled = Error::internal("Command handler did not complete");
        try {
            handled = handler(message.payload, context);
        } catch (const std::exception& error) {
            handled = Error::internal(std::string("Command handler threw: ") +
                                      error.what());
        } catch (...) {
            handled = Error::internal("Command handler threw an unknown exception");
        }
        if (!handled) {
            auto error = handled.error();
            error.operation_id = record.operation_id;
            auto failed = journal_->updateState(record.identity, record.operation_id,
                                                OperationState::Failed, error);
            if (!failed) {
                sendError(message, adapter, failed.error());
                return;
            }
            sendError(message, adapter, std::move(error));
            return;
        }
        sendCommandAck(message, record, decision.duplicate, adapter);
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
    std::shared_ptr<core::IdempotencyJournal> journal_;
    ServiceInfo local_service_;
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
        state->subscriptions = SubscriptionRegistry::create(adapter);
        state->service_handlers = ServiceHandlerRegistry::create(
            options.idempotency_journal,
            adapter ? adapter->localService() : ServiceInfo{});
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
            if (message.message_type == MessageType::Event ||
                message.message_type == MessageType::Progress ||
                message.message_type == MessageType::Log ||
                message.message_type == MessageType::Error) {
                current->subscriptions->dispatch(message);
                return;
            }
            if (message.message_type == MessageType::Command ||
                message.message_type == MessageType::Request) {
                current->service_handlers->dispatch(message, adapter);
                return;
            }
            if (message.message_type != MessageType::Reply) {
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
                    current->subscriptions->resubscribeAll();
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
    if (options.service_name.empty()) {
        return Error::invalidArgument("ClientOptions.service_name is required");
    }
    if (auto existing = g_process_client.lock()) {
        const auto state = existing->connectionState();
        if (state != ConnectionState::Disconnected &&
            state != ConnectionState::ShuttingDown) {
            return existing;
        }
    }

    ClientOptions resolved_options = options;
    if (!resolved_options.runtime_adapter) {
#if defined(INDUSTRIAL_BUS_HAS_LINUX_RUNTIME)
        resolved_options.runtime_adapter =
            client::makeLinuxRuntimeAdapter(resolved_options);
#else
        return Error::serviceUnavailable(
            "this build has no default Linux runtime adapter");
#endif
    }
    auto impl = std::make_unique<Impl>(resolved_options);
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
                                   PublishOptions options,
                                   MessageClass topic_class) {
    if (topic.empty() || payload_type.empty()) {
        return Error::invalidArgument("event topic and payload type must not be empty");
    }
    if (!impl_->state->accepting.load() ||
        impl_->state->connection.load() != ConnectionState::Connected) {
        return Error::connectionLost("client is not connected");
    }
    const auto message_class = options.message_class.value_or(topic_class);
    if (!isPublishableMessageClass(message_class)) {
        return Error::invalidArgument(
            "publish requires an Event, Progress, FinalState or Log class");
    }
    if ((message_class == MessageClass::Progress ||
         message_class == MessageClass::FinalState) &&
        options.operation_id.empty()) {
        return Error::invalidArgument(
            "progress and final-state publication require an operation id");
    }
    client::OutboundMessage message;
    applySemantics(message, message_class);
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
    return impl_->state->subscriptions->add(std::move(topic), std::move(payload_type),
                                             std::move(handler), std::move(executor));
}

Subscription BusClient::registerHandlerRaw(bool command_handler,
                                           std::string topic,
                                           std::string input_type,
                                           std::string output_type,
                                           RawPayloadValidator validator,
                                           RawServiceHandler handler,
                                           std::shared_ptr<IExecutor> executor) {
    if (topic.empty() || input_type.empty() || output_type.empty() || !handler ||
        !impl_->state->accepting.load()) {
        return {};
    }
    if (command_handler &&
        output_type != "industrial_bus.system.v1.CommandAck") {
        return {};
    }
    if (!executor) {
        executor = impl_->executor;
    }
    return impl_->state->service_handlers->add(
        command_handler, std::move(topic), std::move(input_type),
        std::move(output_type), std::move(validator), std::move(handler),
        std::move(executor));
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
    applySemantics(message, MessageClass::Command);
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
    applySemantics(message, MessageClass::Request);
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
    applySemantics(message, isTerminal(update.state) ? MessageClass::FinalState
                                                     : MessageClass::Event);
    message.payload_role = client::PayloadRole::OperationData;
    message.message_id = generateUuidV4();
    message.correlation_id = correlation_id.empty() ? generateUuidV4()
                                                     : std::move(correlation_id);
    message.operation_id = update.operation_id;
    message.topic = "system.operation.event";
    message.payload_type = operation::v1::OperationEvent::descriptor()->full_name();
    message.operation = std::make_shared<OperationUpdate>(std::move(update));
    return impl_->adapter->send(std::move(message));
}

Result<void> BusClient::reportProgress(OperationUpdate update,
                                       std::string correlation_id) {
    if (update.operation_id.empty()) {
        return Error::invalidArgument("progress update requires an operation id");
    }
    const auto valid = [](const std::optional<double>& value) {
        return !value || (*value >= 0.0 && *value <= 1.0);
    };
    if (!valid(update.stage_progress) || !valid(update.overall_progress)) {
        return Error::invalidArgument("progress must be between zero and one");
    }
    if (!impl_->state->accepting.load() ||
        impl_->state->connection.load() != ConnectionState::Connected) {
        return Error::connectionLost("client is not connected");
    }

    operation::v1::ProgressEvent progress;
    progress.set_operation_id(update.operation_id);
    progress.set_stage(std::move(update.stage));
    if (update.stage_progress) {
        progress.set_stage_progress(*update.stage_progress);
    }
    if (update.overall_progress) {
        progress.set_overall_progress(*update.overall_progress);
    }
    if (update.current) {
        progress.set_current(*update.current);
    }
    if (update.total) {
        progress.set_total(*update.total);
    }
    progress.set_message(std::move(update.message));

    ByteBuffer payload;
    if (!PayloadCodec<operation::v1::ProgressEvent>::encode(progress, payload)) {
        return Error::protocol("failed to serialize ProgressEvent");
    }

    client::OutboundMessage message;
    applySemantics(message, MessageClass::Progress);
    message.payload_role = client::PayloadRole::EventData;
    message.message_id = generateUuidV4();
    message.correlation_id = correlation_id.empty() ? generateUuidV4()
                                                     : std::move(correlation_id);
    message.operation_id = update.operation_id;
    message.topic = "system.operation.progress";
    message.payload_type =
        operation::v1::ProgressEvent::descriptor()->full_name();
    message.payload = std::move(payload);
    return impl_->adapter->send(std::move(message));
}

}  // namespace industrial_bus
