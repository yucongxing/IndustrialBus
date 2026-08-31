#pragma once

#include "industrial_bus/client_options.h"
#include "industrial_bus/destination.h"
#include "industrial_bus/executor.h"
#include "industrial_bus/future.h"
#include "industrial_bus/operation.h"
#include "industrial_bus/payload_codec.h"
#include "industrial_bus/result.h"
#include "industrial_bus/subscription.h"
#include "industrial_bus/topic.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace industrial_bus {

struct PublishOptions {
    std::string correlation_id;
    std::string causation_id;
    std::string operation_id;
};

struct CommandOptions {
    std::string idempotency_key;
    Duration accept_timeout{std::chrono::seconds(5)};
    std::string correlation_id;
    std::string operation_id;
};

struct MessageContext {
    std::string message_id;
    std::string correlation_id;
    std::string operation_id;
    std::string source_service;
    std::string source_instance;
    std::string idempotency_key;
    std::int64_t deadline_unix_ns{0};
};

class BusClient : public std::enable_shared_from_this<BusClient> {
public:
    static Result<std::shared_ptr<BusClient>> connect(const ClientOptions& options);

    ~BusClient();
    BusClient(const BusClient&) = delete;
    BusClient& operator=(const BusClient&) = delete;

    template <typename Event>
    Result<void> publish(EventTopic<Event> topic,
                         const Event& event,
                         PublishOptions options = {}) {
        ByteBuffer bytes;
        if (!PayloadCodec<Event>::encode(event, bytes)) {
            return Error::protocol("failed to serialize event payload");
        }
        return publishRaw(std::string(topic.name), std::string(topic.payload_type),
                          std::move(bytes), std::move(options));
    }

    template <typename Event, typename Handler>
    Subscription subscribe(EventTopic<Event> topic,
                           Handler&& handler,
                           std::shared_ptr<IExecutor> executor = {}) {
        using HandlerType = typename std::decay<Handler>::type;
        auto shared_handler = std::make_shared<HandlerType>(std::forward<Handler>(handler));
        return subscribeRaw(
            std::string(topic.name), std::string(topic.payload_type),
            [shared_handler](const ByteBuffer& bytes) {
                Event event;
                if (PayloadCodec<Event>::decode(bytes, event)) {
                    (*shared_handler)(event);
                }
            },
            std::move(executor));
    }

    template <typename Command, typename Ack>
    Future<Result<Ack>> command(Destination destination,
                                CommandTopic<Command, Ack> topic,
                                const Command& command_value,
                                CommandOptions options = {}) {
        auto promise = std::make_shared<std::promise<Result<Ack>>>();
        auto future = promise->get_future();
        ByteBuffer bytes;
        if (!PayloadCodec<Command>::encode(command_value, bytes)) {
            promise->set_value(Error::protocol("failed to serialize command payload"));
            return future;
        }
        commandRaw(
            std::move(destination), std::string(topic.name),
            std::string(topic.command_type), std::string(topic.ack_type),
            std::move(bytes), std::move(options),
            [promise](Result<ByteBuffer> raw) mutable {
                completeTypedPromise<Ack>(std::move(promise), std::move(raw));
            });
        return future;
    }

    template <typename Request, typename Response>
    Future<Result<Response>> request(Destination destination,
                                     RequestTopic<Request, Response> topic,
                                     const Request& request_value,
                                     Duration timeout) {
        auto promise = std::make_shared<std::promise<Result<Response>>>();
        auto future = promise->get_future();
        ByteBuffer bytes;
        if (!PayloadCodec<Request>::encode(request_value, bytes)) {
            promise->set_value(Error::protocol("failed to serialize request payload"));
            return future;
        }
        requestRaw(
            std::move(destination), std::string(topic.name),
            std::string(topic.request_type), std::string(topic.response_type),
            std::move(bytes), timeout,
            [promise](Result<ByteBuffer> raw) mutable {
                completeTypedPromise<Response>(std::move(promise), std::move(raw));
            });
        return future;
    }

    template <typename Command, typename Ack, typename Handler>
    Subscription handleCommand(CommandTopic<Command, Ack> topic,
                               Handler&& handler,
                               std::shared_ptr<IExecutor> executor = {}) {
        using HandlerType = typename std::decay<Handler>::type;
        auto shared_handler =
            std::make_shared<HandlerType>(std::forward<Handler>(handler));
        return registerHandlerRaw(
            true, std::string(topic.name), std::string(topic.command_type),
            std::string(topic.ack_type),
            [shared_handler](const ByteBuffer& bytes,
                             const MessageContext& context) -> Result<ByteBuffer> {
                Command command_value;
                if (!PayloadCodec<Command>::decode(bytes, command_value)) {
                    return Error::protocol("failed to parse command payload");
                }
                Result<Ack> result = (*shared_handler)(command_value, context);
                if (!result) {
                    return result.error();
                }
                ByteBuffer response;
                if (!PayloadCodec<Ack>::encode(result.value(), response)) {
                    return Error::protocol("failed to serialize CommandAck payload");
                }
                return response;
            },
            std::move(executor));
    }

    template <typename Request, typename Response, typename Handler>
    Subscription handleRequest(RequestTopic<Request, Response> topic,
                               Handler&& handler,
                               std::shared_ptr<IExecutor> executor = {}) {
        using HandlerType = typename std::decay<Handler>::type;
        auto shared_handler =
            std::make_shared<HandlerType>(std::forward<Handler>(handler));
        return registerHandlerRaw(
            false, std::string(topic.name), std::string(topic.request_type),
            std::string(topic.response_type),
            [shared_handler](const ByteBuffer& bytes,
                             const MessageContext& context) -> Result<ByteBuffer> {
                Request request_value;
                if (!PayloadCodec<Request>::decode(bytes, request_value)) {
                    return Error::protocol("failed to parse request payload");
                }
                Result<Response> result = (*shared_handler)(request_value, context);
                if (!result) {
                    return result.error();
                }
                ByteBuffer response;
                if (!PayloadCodec<Response>::encode(result.value(), response)) {
                    return Error::protocol("failed to serialize response payload");
                }
                return response;
            },
            std::move(executor));
    }

    Result<void> shutdown(Duration timeout);
    ConnectionState connectionState() const;
    ServiceSnapshot services() const;
    Result<void> reportOperation(OperationUpdate update,
                                 std::string correlation_id = {});

    // Used by integration helpers. Applications normally use typed methods above.
    Result<void> publishRaw(std::string topic,
                            std::string payload_type,
                            ByteBuffer payload,
                            PublishOptions options = {});

private:
    using RawCompletion = std::function<void(Result<ByteBuffer>)>;
    using RawEventHandler = std::function<void(const ByteBuffer&)>;
    using RawServiceHandler =
        std::function<Result<ByteBuffer>(const ByteBuffer&, const MessageContext&)>;

    class Impl;
    explicit BusClient(std::unique_ptr<Impl> impl);

    Subscription subscribeRaw(std::string topic,
                              std::string payload_type,
                              RawEventHandler handler,
                              std::shared_ptr<IExecutor> executor);
    Subscription registerHandlerRaw(bool command_handler,
                                    std::string topic,
                                    std::string input_type,
                                    std::string output_type,
                                    RawServiceHandler handler,
                                    std::shared_ptr<IExecutor> executor);
    void commandRaw(Destination destination,
                    std::string topic,
                    std::string command_type,
                    std::string ack_type,
                    ByteBuffer payload,
                    CommandOptions options,
                    RawCompletion completion);
    void requestRaw(Destination destination,
                    std::string topic,
                    std::string request_type,
                    std::string response_type,
                    ByteBuffer payload,
                    Duration timeout,
                    RawCompletion completion);

    template <typename T>
    static void completeTypedPromise(std::shared_ptr<std::promise<Result<T>>> promise,
                                     Result<ByteBuffer> raw) {
        if (!raw) {
            promise->set_value(raw.error());
            return;
        }
        T value;
        if (!PayloadCodec<T>::decode(raw.value(), value)) {
            promise->set_value(Error::protocol("failed to parse reply payload"));
            return;
        }
        promise->set_value(std::move(value));
    }

    std::unique_ptr<Impl> impl_;
};

}  // namespace industrial_bus
