#include "industrial_bus/bus_client.h"
#include "industrial_bus/core/idempotency_journal.h"
#include "industrial_bus/ids.h"
#include "industrial_bus/runtime/commhub_server.h"
#include "industrial_bus/runtime/envelope_codec.h"
#include "industrial_bus/runtime/transport_runtime_adapter.h"
#include "industrial_bus/topics.generated.h"
#include "loopback_transport.h"
#include "test_topic_catalog.h"

#include "cmp.pb.h"
#include "envelope.pb.h"
#include "operation.pb.h"
#include "system.pb.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace industrial_bus;

class CapturingRuntimeAdapter final : public client::IRuntimeAdapter {
public:
    Result<void> start() override {
        state_ = ConnectionState::Connected;
        if (connection_handler_) {
            connection_handler_(state_, 1);
        }
        return {};
    }

    Result<void> send(client::OutboundMessage message) override {
        sent_message_ = std::move(message);
        has_sent_message_ = true;
        return {};
    }

    void setReceiveHandler(ReceiveHandler handler) override {
        receive_handler_ = std::move(handler);
    }

    void setConnectionHandler(ConnectionHandler handler) override {
        connection_handler_ = std::move(handler);
    }

    ConnectionState state() const override { return state_; }
    std::uint64_t generation() const override { return 1; }
    ServiceSnapshot services() const override { return {}; }
    bool flush(std::chrono::milliseconds) override { return true; }
    void close() noexcept override { state_ = ConnectionState::Disconnected; }

    bool hasSentMessage() const { return has_sent_message_; }
    const client::OutboundMessage& sentMessage() const { return sent_message_; }

private:
    ReceiveHandler receive_handler_;
    ConnectionHandler connection_handler_;
    ConnectionState state_{ConnectionState::Disconnected};
    client::OutboundMessage sent_message_;
    bool has_sent_message_{false};
};

class SendFailureController {
public:
    void arm(std::size_t connection_index, std::string topic) {
        connection_index_ = connection_index;
        topic_ = std::move(topic);
        armed_.store(true);
    }

    bool shouldFail(std::size_t connection_index, transport::ByteView frame) {
        if (!armed_.load() || connection_index != connection_index_) {
            return false;
        }
        protocol::v1::MessageEnvelope wire;
        if (!wire.ParseFromArray(frame.data(), static_cast<int>(frame.size())) ||
            wire.topic() != topic_) {
            return false;
        }
        ++failure_count_;
        return true;
    }

    int failureCount() const { return failure_count_.load(); }

private:
    std::size_t connection_index_{0};
    std::string topic_;
    std::atomic<bool> armed_{false};
    std::atomic<int> failure_count_{0};
};

class SelectivelyFailingConnection final : public transport::IConnection {
public:
    SelectivelyFailingConnection(
        std::shared_ptr<transport::IConnection> inner,
        std::shared_ptr<SendFailureController> failure_controller,
        std::size_t connection_index)
        : inner_(std::move(inner)),
          failure_controller_(std::move(failure_controller)),
          connection_index_(connection_index) {}

    transport::Result<void> send(transport::ByteView frame) override {
        if (failure_controller_->shouldFail(connection_index_, frame)) {
            return transport::Result<void>::failure(
                {transport::ErrorCode::transport_failure,
                 "injected subscriber send failure"});
        }
        return inner_->send(frame);
    }

    void setReceiveHandler(transport::ReceiveHandler handler) override {
        inner_->setReceiveHandler(std::move(handler));
    }

    transport::ConnectionState state() const override { return inner_->state(); }
    void close() override { inner_->close(); }

private:
    std::shared_ptr<transport::IConnection> inner_;
    std::shared_ptr<SendFailureController> failure_controller_;
    std::size_t connection_index_;
};

class SelectivelyFailingListener final : public transport::IListener {
public:
    SelectivelyFailingListener()
        : inner_(std::make_shared<transport::LoopbackHub>()),
          failure_controller_(std::make_shared<SendFailureController>()) {}

    transport::Result<void> listen(
        const transport::Endpoint& endpoint,
        transport::AcceptHandler handler) override {
        return inner_->listen(
            endpoint,
            [this, handler = std::move(handler)](
                std::shared_ptr<transport::IConnection> connection) mutable {
                const auto connection_index = next_connection_index_++;
                handler(std::make_shared<SelectivelyFailingConnection>(
                    std::move(connection), failure_controller_, connection_index));
            });
    }

    void close() override { inner_->close(); }

    std::shared_ptr<transport::IConnector> connector() const { return inner_; }
    std::shared_ptr<SendFailureController> failureController() const {
        return failure_controller_;
    }

private:
    std::shared_ptr<transport::LoopbackHub> inner_;
    std::shared_ptr<SendFailureController> failure_controller_;
    std::atomic<std::size_t> next_connection_index_{0};
};

transport::ByteBuffer serializeEnvelope(
    const protocol::v1::MessageEnvelope& wire) {
    std::string serialized;
    if (!wire.SerializeToString(&serialized)) {
        return {};
    }
    return transport::ByteBuffer(serialized.begin(), serialized.end());
}

protocol::v1::MessageEnvelope codecEnvelope(
    protocol::v1::MessageType message_type,
    protocol::v1::PayloadRole payload_role,
    std::uint32_t priority) {
    protocol::v1::MessageEnvelope wire;
    wire.set_message_id("codec-message-" +
                        std::to_string(static_cast<int>(message_type)));
    wire.set_correlation_id("codec-correlation");
    wire.set_source_node("node");
    wire.set_source_service("codec-source");
    wire.set_source_instance("codec-instance");
    wire.set_topic("test.codec");
    wire.set_message_type(message_type);
    wire.set_payload_role(payload_role);
    wire.set_payload_type("test.Bytes");
    wire.set_priority(priority);
    wire.set_protocol_version(1);
    wire.set_schema_version(1);
    wire.set_max_hops(1);
    const auto now = std::chrono::system_clock::now();
    wire.set_timestamp_unix_ns(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch())
            .count());
    wire.set_payload("payload");
    if (message_type == protocol::v1::COMMAND ||
        message_type == protocol::v1::REQUEST) {
        wire.set_destination_service("worker");
        wire.set_deadline_unix_ns(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                (now + std::chrono::hours(1)).time_since_epoch())
                .count());
    }
    if (message_type == protocol::v1::COMMAND) {
        wire.set_idempotency_key("codec-intent");
    }
    if (message_type == protocol::v1::REPLY) {
        wire.set_destination_service("caller");
        wire.set_causation_id("codec-request");
    }
    if (message_type == protocol::v1::PROGRESS) {
        wire.set_operation_id("codec-operation");
    }
    return wire;
}

client::OutboundMessage outboundFromInbound(
    const client::InboundMessage& inbound) {
    client::OutboundMessage outbound;
    outbound.message_type = inbound.message_type;
    outbound.priority = inbound.priority;
    outbound.message_class = inbound.message_class;
    outbound.payload_role = inbound.payload_role;
    outbound.message_id = inbound.message_id;
    outbound.correlation_id = inbound.correlation_id;
    outbound.causation_id = inbound.causation_id;
    outbound.operation_id = inbound.operation_id;
    outbound.topic = inbound.topic;
    outbound.payload_type = inbound.payload_type;
    outbound.payload = inbound.payload;
    outbound.idempotency_key = inbound.idempotency_key;
    outbound.deadline_unix_ns = inbound.deadline_unix_ns;
    if (inbound.message_type == MessageType::Command ||
        inbound.message_type == MessageType::Request) {
        outbound.destination.service_name = "worker";
    } else if (inbound.message_type == MessageType::Reply) {
        outbound.destination.service_name = "caller";
    }
    if (inbound.has_error) {
        outbound.error = std::make_shared<Error>(inbound.error);
    }
    return outbound;
}

std::int64_t deadlineAfter(std::chrono::seconds duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               (std::chrono::system_clock::now() + duration).time_since_epoch())
        .count();
}
client::OutboundMessage eventMessage(std::string topic, std::string body) {
    client::OutboundMessage message;
    message.message_type = MessageType::Event;
    message.payload_role = client::PayloadRole::EventData;
    message.message_id = generateUuidV4();
    message.correlation_id = generateUuidV4();
    message.topic = std::move(topic);
    message.payload_type = "test.StringEvent";
    message.payload.assign(body.begin(), body.end());
    return message;
}

bool hasService(const ServiceSnapshot& services,
                const std::string& name,
                const std::string& instance) {
    return std::any_of(services.begin(), services.end(), [&](const auto& service) {
        return service.service_name == name && service.instance_id == instance;
    });
}

TEST(EnvelopeCodecTest, RoundTripsEverySupportedMessageTypeExactly) {
    struct MessageTypeCase {
        protocol::v1::MessageType message_type;
        protocol::v1::PayloadRole payload_role;
        std::uint32_t legacy_priority;
    };
    const std::array<MessageTypeCase, 9> cases{{
        {protocol::v1::COMMAND, protocol::v1::COMMAND_INPUT, 1},
        {protocol::v1::EVENT, protocol::v1::EVENT_FACT, 2},
        {protocol::v1::REQUEST, protocol::v1::REQUEST_INPUT, 1},
        {protocol::v1::REPLY, protocol::v1::RESPONSE_OUTPUT, 0},
        {protocol::v1::PROGRESS, protocol::v1::EVENT_FACT, 2},
        {protocol::v1::ERROR, protocol::v1::ERROR_DETAIL, 0},
        {protocol::v1::LOG, protocol::v1::EVENT_FACT, 2},
        {protocol::v1::HEARTBEAT, protocol::v1::SYSTEM_PAYLOAD, 1},
        {protocol::v1::SYSTEM, protocol::v1::SYSTEM_PAYLOAD, 1},
    }};

    for (const auto& test_case : cases) {
        SCOPED_TRACE(static_cast<int>(test_case.message_type));
        auto original = codecEnvelope(test_case.message_type,
                                      test_case.payload_role,
                                      test_case.legacy_priority);
        if (test_case.message_type == protocol::v1::ERROR) {
            system::v1::Error error;
            error.set_category(system::v1::PROTOCOL_ERROR);
            error.set_message("asynchronous error");
            ASSERT_TRUE(error.SerializeToString(original.mutable_payload()));
            original.set_payload_type(error.GetDescriptor()->full_name());
        }

        const auto validator = runtime::test::protocolValidator();
        auto decoded = runtime::decodeInbound(serializeEnvelope(original), validator);
        ASSERT_TRUE(decoded) << decoded.error().message;
        auto reencoded = runtime::encodeOutbound(
            outboundFromInbound(decoded.value()),
            runtime::SourceIdentity{"node", "codec-source", "codec-instance"},
            validator);
        ASSERT_TRUE(reencoded) << reencoded.error().message;

        protocol::v1::MessageEnvelope round_trip;
        ASSERT_TRUE(round_trip.ParseFromArray(
            reencoded.value().data(), static_cast<int>(reencoded.value().size())));
        EXPECT_EQ(round_trip.message_type(), test_case.message_type);
    }
}

TEST(EnvelopeCodecTest, WritesCanonicalPriorityAndSemanticClassHeader) {
    struct OutboundCase {
        MessageType message_type;
        client::PayloadRole payload_role;
        MessageClass message_class;
        std::uint32_t priority;
    };
    const std::array<OutboundCase, 4> cases{{
        {MessageType::Event, client::PayloadRole::EventData, MessageClass::Event, 2},
        {MessageType::Command, client::PayloadRole::CommandData, MessageClass::Command, 1},
        {MessageType::Request, client::PayloadRole::RequestData, MessageClass::Request, 1},
        {MessageType::Reply, client::PayloadRole::ResponseData, MessageClass::Reply, 0},
    }};

    for (const auto& test_case : cases) {
        SCOPED_TRACE(static_cast<int>(test_case.message_type));
        client::OutboundMessage outbound;
        outbound.message_type = test_case.message_type;
        outbound.message_class = test_case.message_class;
        outbound.priority = defaultPriority(test_case.message_class);
        outbound.payload_role = test_case.payload_role;
        outbound.message_id = generateUuidV4();
        outbound.correlation_id = generateUuidV4();
        outbound.topic = "test.codec.metadata";
        outbound.payload_type = "test.Bytes";
        outbound.payload = {'x'};
        if (test_case.message_type == MessageType::Command ||
            test_case.message_type == MessageType::Request) {
            outbound.destination.service_name = "worker";
            outbound.deadline_unix_ns = deadlineAfter(std::chrono::hours(1));
        }
        if (test_case.message_type == MessageType::Command) {
            outbound.idempotency_key = "metadata-intent";
        }
        if (test_case.message_type == MessageType::Reply) {
            outbound.destination.service_name = "caller";
            outbound.causation_id = "metadata-request";
        }

        auto encoded = runtime::encodeOutbound(
            outbound,
            runtime::SourceIdentity{"node", "codec-source", "codec-instance"},
            runtime::test::protocolValidator());
        ASSERT_TRUE(encoded) << encoded.error().message;
        protocol::v1::MessageEnvelope wire;
        ASSERT_TRUE(wire.ParseFromArray(encoded.value().data(),
                                        static_cast<int>(encoded.value().size())));
        EXPECT_EQ(wire.priority(), test_case.priority);
        const auto semantic_class =
            wire.headers().find("industrial_bus.message_class");
        EXPECT_NE(semantic_class, wire.headers().end());
        if (semantic_class != wire.headers().end()) {
            EXPECT_FALSE(semantic_class->second.empty());
        }
    }
}

TEST(EnvelopeCodecTest, RejectsInvalidTypePriorityAndSemanticMetadata) {
    const auto expect_rejected = [](protocol::v1::MessageEnvelope wire) {
        const auto frame = serializeEnvelope(wire);
        const auto validator = runtime::test::protocolValidator();
        EXPECT_FALSE(runtime::decodeInbound(frame, validator));
        EXPECT_FALSE(runtime::decodeRouteMessage(frame, validator));
    };

    expect_rejected(codecEnvelope(protocol::v1::MESSAGE_TYPE_UNSPECIFIED,
                                  protocol::v1::EVENT_FACT, 2));
    expect_rejected(
        codecEnvelope(protocol::v1::EVENT, protocol::v1::EVENT_FACT, 4));

    auto inconsistent =
        codecEnvelope(protocol::v1::PROGRESS, protocol::v1::EVENT_FACT, 0);
    (*inconsistent.mutable_headers())["industrial_bus.message_class"] =
        "Progress";
    expect_rejected(std::move(inconsistent));

    auto invalid_class =
        codecEnvelope(protocol::v1::EVENT, protocol::v1::EVENT_FACT, 2);
    (*invalid_class.mutable_headers())["industrial_bus.message_class"] =
        "not-a-message-class";
    expect_rejected(std::move(invalid_class));
}

TEST(EnvelopeCodecTest, AppliesSafeFallbackToLegacyHeaderlessMessage) {
    auto legacy = codecEnvelope(protocol::v1::COMMAND,
                                protocol::v1::COMMAND_INPUT, 0);
    ASSERT_TRUE(legacy.headers().empty());
    const auto frame = serializeEnvelope(legacy);

    const auto validator = runtime::test::protocolValidator();
    auto route = runtime::decodeRouteMessage(frame, validator);
    ASSERT_TRUE(route) << route.error().message;
    EXPECT_EQ(route.value().priority, 1U);

    auto inbound = runtime::decodeInbound(frame, validator);
    ASSERT_TRUE(inbound) << inbound.error().message;
    auto encoded = runtime::encodeOutbound(
        outboundFromInbound(inbound.value()),
        runtime::SourceIdentity{"node", "codec-source", "codec-instance"},
        validator);
    ASSERT_TRUE(encoded) << encoded.error().message;
    protocol::v1::MessageEnvelope normalized;
    ASSERT_TRUE(normalized.ParseFromArray(
        encoded.value().data(), static_cast<int>(encoded.value().size())));
    EXPECT_EQ(normalized.message_type(), protocol::v1::COMMAND);
    EXPECT_EQ(normalized.priority(), 1U);
    EXPECT_NE(normalized.headers().find("industrial_bus.message_class"),
              normalized.headers().end());
}

TEST(EnvelopeCodecTest, RejectsMismatchedCanonicalProgressOperationIds) {
    auto wire = codecEnvelope(protocol::v1::PROGRESS,
                              protocol::v1::EVENT_FACT, 2);
    wire.set_topic("system.operation.progress");
    wire.set_operation_id("envelope-operation");
    wire.set_payload_type(
        operation::v1::ProgressEvent::descriptor()->full_name());
    (*wire.mutable_headers())["industrial_bus.message_class"] = "Progress";
    operation::v1::ProgressEvent progress;
    progress.set_operation_id("payload-operation");
    ASSERT_TRUE(progress.SerializeToString(wire.mutable_payload()));

    const auto frame = serializeEnvelope(wire);
    EXPECT_FALSE(runtime::decodeInbound(frame, runtime::test::protocolValidator()));
    EXPECT_FALSE(runtime::decodeRouteMessage(frame,
                                             runtime::test::protocolValidator()));
}

TEST(EnvelopeCodecTest, PreservesStructuredError) {
    client::OutboundMessage reply;
    reply.message_type = MessageType::Reply;
    reply.message_class = MessageClass::Error;
    reply.priority = QosPriority::Critical;
    reply.payload_role = client::PayloadRole::ErrorDetail;
    reply.message_id = "error-message";
    reply.correlation_id = "correlation";
    reply.causation_id = "request-message";
    reply.operation_id = "operation";
    reply.destination.service_name = "caller";
    reply.destination.instance_id = "caller-instance";
    reply.topic = "test.request";
    reply.payload_type = "industrial_bus.system.v1.Error";
    reply.error = std::make_shared<Error>(
        Error::idempotencyConflict("intent payload mismatch", 42));
    reply.error->operation_id = "operation";
    reply.error->context["key"] = "intent-1";

    auto encoded = runtime::encodeOutbound(
        reply, runtime::SourceIdentity{"node", "worker", "worker-instance"},
        runtime::test::protocolValidator());
    ASSERT_TRUE(encoded);
    auto decoded = runtime::decodeInbound(encoded.value(),
                                           runtime::test::protocolValidator());
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded.value().message_type == MessageType::Reply);
    EXPECT_TRUE(decoded.value().payload_role == client::PayloadRole::ErrorDetail);
    EXPECT_TRUE(decoded.value().source_service == "worker");
    EXPECT_TRUE(decoded.value().source_instance == "worker-instance");
    EXPECT_TRUE(decoded.value().has_error);
    EXPECT_TRUE(decoded.value().error == *reply.error);
}

TEST(EnvelopeCodecTest, PreservesCanonicalOperationEventWireContract) {
    auto adapter = std::make_shared<CapturingRuntimeAdapter>();
    ClientOptions options;
    options.service_name = "operation-codec-test";
    options.runtime_adapter = adapter;
    auto connected = BusClient::connect(options);
    ASSERT_TRUE(connected) << connected.error().message;
    auto bus = connected.value();

    OperationUpdate update;
    update.operation_id = "operation-1";
    update.operation_type = "test.operation";
    update.state = OperationState::Running;
    update.stage = "processing";
    update.stage_progress = 0.25;
    update.overall_progress = 0.5;
    ASSERT_TRUE(bus->reportOperation(std::move(update), "correlation-1"));
    ASSERT_TRUE(adapter->hasSentMessage());

    auto encoded = runtime::encodeOutbound(
        adapter->sentMessage(),
        runtime::SourceIdentity{"node", "operation-codec-test", "instance"});
    ASSERT_TRUE(encoded) << encoded.error().message;

    protocol::v1::MessageEnvelope wire;
    ASSERT_TRUE(wire.ParseFromArray(encoded.value().data(),
                                    static_cast<int>(encoded.value().size())));
    EXPECT_EQ(wire.message_type(), protocol::v1::EVENT);
    EXPECT_EQ(wire.payload_role(), protocol::v1::EVENT_FACT);
    EXPECT_EQ(wire.payload_type(),
              operation::v1::OperationEvent::descriptor()->full_name());
    EXPECT_EQ(wire.topic(), "system.operation.event");
    EXPECT_EQ(wire.protocol_version(), 1U);
    EXPECT_EQ(wire.schema_version(), 1U);

    operation::v1::OperationEvent event;
    ASSERT_TRUE(event.ParseFromString(wire.payload()));
    EXPECT_EQ(event.operation_id(), "operation-1");
    EXPECT_EQ(event.operation_type(), "test.operation");
    EXPECT_EQ(event.state(), operation::v1::RUNNING);
    EXPECT_EQ(event.stage(), "processing");
    EXPECT_DOUBLE_EQ(event.stage_progress(), 0.25);
    EXPECT_DOUBLE_EQ(event.overall_progress(), 0.5);

    EXPECT_TRUE(bus->shutdown(std::chrono::milliseconds(10)));
}

TEST(RuntimeE2eTest, RoutesEventsCommandsAndErrors) {
    char journal_directory_template[] = "/tmp/industrial-bus-e2e-journal-XXXXXX";
    char* journal_directory = ::mkdtemp(journal_directory_template);
    ASSERT_NE(journal_directory, nullptr);
    const std::string journal_path =
        std::string(journal_directory) + "/commands.journal";
    core::IdempotencyJournalOptions journal_options;
    journal_options.path = journal_path;
    auto opened_journal = core::IdempotencyJournal::open(journal_options);
    ASSERT_TRUE(opened_journal);

    auto loopback = std::make_shared<transport::LoopbackHub>();
    const transport::Endpoint endpoint{"loopback://runtime-e2e"};
    runtime::CommHubServerOptions hub_options;
    hub_options.topic_catalog = runtime::test::topicCatalog();
    runtime::CommHubServer hub(loopback, endpoint, "hub-e2e", "test-token",
                               hub_options);
    ASSERT_TRUE(hub.start());

    runtime::TransportRuntimeOptions rejected_options;
    rejected_options.endpoint = endpoint;
    rejected_options.connector = loopback;
    rejected_options.service_name = "rejected";
    rejected_options.instance_id = "rejected-instance";
    rejected_options.auth_token = "wrong-token";
    rejected_options.handshake_timeout = std::chrono::milliseconds(20);
    rejected_options.topic_catalog = runtime::test::topicCatalog();
    runtime::TransportRuntimeAdapter rejected(rejected_options);
    EXPECT_TRUE(!rejected.start());

    runtime::TransportRuntimeOptions caller_options;
    caller_options.endpoint = endpoint;
    caller_options.connector = loopback;
    caller_options.service_name = "caller";
    caller_options.instance_id = "caller-instance";
    caller_options.auth_token = "test-token";
    caller_options.topic_catalog = runtime::test::topicCatalog();
    auto caller = std::make_shared<runtime::TransportRuntimeAdapter>(caller_options);

    runtime::TransportRuntimeOptions worker_options;
    worker_options.endpoint = endpoint;
    worker_options.connector = loopback;
    worker_options.service_name = "worker";
    worker_options.instance_id = "worker-instance";
    worker_options.auth_token = "test-token";
    worker_options.topic_catalog = runtime::test::topicCatalog();
    auto worker = std::make_shared<runtime::TransportRuntimeAdapter>(worker_options);

    std::mutex mutex;
    std::condition_variable changed;
    std::string caller_event;
    std::string parameterized_operation;
    std::string command_ack;
    bool duplicate_ack = false;
    int ack_count = 0;
    Error route_error;

    caller->setReceiveHandler([&](client::InboundMessage message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (message.has_error) {
            route_error = std::move(message.error);
        } else if (message.payload_role == client::PayloadRole::CommandAck) {
            system::v1::CommandAck ack;
            EXPECT_TRUE(PayloadCodec<system::v1::CommandAck>::decode(message.payload, ack));
            command_ack = ack.operation_id();
            duplicate_ack = ack.duplicate();
            ++ack_count;
        } else if (message.payload_type ==
                   cmp::v1::SimulationCompleted::descriptor()->full_name()) {
            cmp::v1::SimulationCompleted completed;
            EXPECT_TRUE(PayloadCodec<cmp::v1::SimulationCompleted>::decode(
                message.payload, completed));
            parameterized_operation = completed.operation_id();
        } else {
            caller_event.assign(message.payload.begin(), message.payload.end());
        }
        changed.notify_all();
    });

    ASSERT_TRUE(caller->start());
    ClientOptions worker_client_options;
    worker_client_options.service_name = "worker";
    worker_client_options.runtime_adapter = worker;
    worker_client_options.idempotency_journal =
        std::shared_ptr<core::IdempotencyJournal>(std::move(opened_journal.value()));
    auto connected_worker = BusClient::connect(worker_client_options);
    ASSERT_TRUE(connected_worker);
    auto worker_bus = connected_worker.value();
    const CommandTopic<std::string, system::v1::CommandAck> command_topic{
        "test.command", "test.StringCommand",
        "industrial_bus.system.v1.CommandAck"};
    std::atomic<int> command_executions{0};
    auto command_handler = worker_bus->handleCommand(
        command_topic,
        [&](const std::string& value, const MessageContext&) -> Result<void> {
            EXPECT_TRUE(value == "execute");
            ++command_executions;
            return {};
        });
    EXPECT_TRUE(command_handler);

    EXPECT_TRUE(caller->subscribe("test.event"));
    EXPECT_TRUE(worker->send(eventMessage("test.event", "broadcast")));
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return caller_event == "broadcast";
        }));
    }

    const topics::ChannelTopics channel_topics(
        "123e4567-e89b-42d3-a456-426614174000");
    const auto completed_topic = channel_topics.simulationCompleted();
    EXPECT_TRUE(caller->subscribe(std::string(completed_topic.name)));
    cmp::v1::SimulationCompleted completed;
    completed.set_operation_id("parameterized-operation");
    PublishOptions completed_options;
    completed_options.operation_id = completed.operation_id();
    const auto published = worker_bus->publish(
        completed_topic, completed, std::move(completed_options));
    ASSERT_TRUE(published) << published.error().message;
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return parameterized_operation == "parameterized-operation";
        }));
    }

    client::OutboundMessage command;
    command.message_type = MessageType::Command;
    command.message_class = MessageClass::Command;
    command.priority = QosPriority::High;
    command.payload_role = client::PayloadRole::CommandData;
    command.message_id = generateUuidV4();
    command.correlation_id = generateUuidV4();
    const auto command_operation_id = generateUuidV4();
    command.operation_id = command_operation_id;
    command.destination = {"worker", {}};
    command.topic = "test.command";
    command.payload_type = "test.StringCommand";
    command.idempotency_key = "runtime-e2e-command";
    command.deadline_unix_ns = deadlineAfter(std::chrono::seconds(2));
    PayloadCodec<std::string>::encode("execute", command.payload);
    EXPECT_TRUE(caller->send(std::move(command)));
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return command_ack == command_operation_id && ack_count == 1;
        }));
    }
    EXPECT_TRUE(command_executions == 1);
    EXPECT_TRUE(!duplicate_ack);

    client::OutboundMessage duplicate_command;
    duplicate_command.message_type = MessageType::Command;
    duplicate_command.message_class = MessageClass::Command;
    duplicate_command.priority = QosPriority::High;
    duplicate_command.payload_role = client::PayloadRole::CommandData;
    duplicate_command.message_id = generateUuidV4();
    duplicate_command.correlation_id = generateUuidV4();
    duplicate_command.operation_id = command_operation_id;
    duplicate_command.destination = {"worker", {}};
    duplicate_command.topic = "test.command";
    duplicate_command.payload_type = "test.StringCommand";
    duplicate_command.idempotency_key = "runtime-e2e-command";
    duplicate_command.deadline_unix_ns = deadlineAfter(std::chrono::seconds(2));
    PayloadCodec<std::string>::encode("execute", duplicate_command.payload);
    EXPECT_TRUE(caller->send(std::move(duplicate_command)));
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return ack_count == 2 && duplicate_ack;
        }));
    }
    EXPECT_TRUE(command_executions == 1);

    client::OutboundMessage missing;
    missing.message_type = MessageType::Command;
    missing.message_class = MessageClass::Command;
    missing.priority = QosPriority::High;
    missing.payload_role = client::PayloadRole::CommandData;
    missing.message_id = generateUuidV4();
    missing.correlation_id = generateUuidV4();
    missing.destination = {"missing-service", {}};
    missing.topic = "test.missing";
    missing.payload_type = "test.MissingCommand";
    missing.idempotency_key = "runtime-e2e-missing";
    missing.deadline_unix_ns = deadlineAfter(std::chrono::seconds(2));
    EXPECT_TRUE(caller->send(std::move(missing)));
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return route_error.category == ErrorCategory::ServiceUnavailable;
        }));
    }

    EXPECT_TRUE(worker_bus->shutdown(std::chrono::milliseconds(20)));
    caller->close();
    hub.stop();
    EXPECT_TRUE(::unlink(journal_path.c_str()) == 0);
    EXPECT_TRUE(::rmdir(journal_directory) == 0);
}

TEST(RuntimeE2eTest, DiscoversMultipleInstancesAndRemovesClosedServices) {
    auto loopback = std::make_shared<transport::LoopbackHub>();
    const transport::Endpoint endpoint{"loopback://runtime-directory"};
    runtime::CommHubServer hub(loopback, endpoint, "hub-directory", "token");
    ASSERT_TRUE(hub.start());

    auto make_options = [&](std::string service, std::string instance) {
        runtime::TransportRuntimeOptions options;
        options.endpoint = endpoint;
        options.connector = loopback;
        options.service_name = std::move(service);
        options.instance_id = std::move(instance);
        options.auth_token = "token";
        options.expected_hub_instance_id = "hub-directory";
        options.instance_mode = InstanceMode::MultiInstance;
        return options;
    };

    runtime::TransportRuntimeAdapter observer(
        make_options("layout-view", "layout-instance"));
    runtime::TransportRuntimeAdapter first(
        make_options("cmp-suite", "channel-a"));
    runtime::TransportRuntimeAdapter second(
        make_options("cmp-suite", "channel-b"));

    ASSERT_TRUE(observer.start());
    ASSERT_TRUE(first.start());
    ASSERT_TRUE(second.start());
    EXPECT_TRUE(hasService(second.services(), "layout-view", "layout-instance"));
    EXPECT_TRUE(hasService(second.services(), "cmp-suite", "channel-a"));
    EXPECT_TRUE(hasService(second.services(), "cmp-suite", "channel-b"));
    EXPECT_TRUE(hasService(observer.services(), "cmp-suite", "channel-a"));
    EXPECT_TRUE(hasService(observer.services(), "cmp-suite", "channel-b"));

    first.close();
    EXPECT_FALSE(hasService(observer.services(), "cmp-suite", "channel-a"));
    EXPECT_TRUE(hasService(observer.services(), "cmp-suite", "channel-b"));

    second.close();
    EXPECT_FALSE(hasService(observer.services(), "cmp-suite", "channel-b"));
    observer.close();
    hub.stop();
}

TEST(RuntimeE2eTest, DisconnectsOnlyFailingBroadcastSubscriber) {
    auto failing_listener = std::make_shared<SelectivelyFailingListener>();
    const transport::Endpoint endpoint{"loopback://runtime-slow-consumer"};
    runtime::CommHubServerOptions hub_options;
    hub_options.topic_catalog = runtime::test::topicCatalog();
    runtime::CommHubServer hub(failing_listener, endpoint, "hub-slow", "token",
                               hub_options);
    ASSERT_TRUE(hub.start());

    const auto make_options = [&](std::string service, std::string instance) {
        runtime::TransportRuntimeOptions options;
        options.endpoint = endpoint;
        options.connector = failing_listener->connector();
        options.service_name = std::move(service);
        options.instance_id = std::move(instance);
        options.auth_token = "token";
        options.expected_hub_instance_id = "hub-slow";
        options.topic_catalog = runtime::test::topicCatalog();
        return options;
    };

    runtime::TransportRuntimeAdapter slow(
        make_options("slow-observer", "slow-instance"));
    runtime::TransportRuntimeAdapter healthy(
        make_options("healthy-observer", "healthy-instance"));
    runtime::TransportRuntimeAdapter publisher(
        make_options("publisher", "publisher-instance"));

    std::atomic<int> healthy_deliveries{0};
    healthy.setReceiveHandler([&](client::InboundMessage message) {
        if (message.topic == "test.isolated-broadcast") {
            ++healthy_deliveries;
        }
    });

    ASSERT_TRUE(slow.start());
    ASSERT_TRUE(healthy.start());
    ASSERT_TRUE(publisher.start());
    ASSERT_TRUE(slow.subscribe("test.isolated-broadcast"));
    ASSERT_TRUE(healthy.subscribe("test.isolated-broadcast"));
    ASSERT_TRUE(hasService(publisher.services(), "slow-observer", "slow-instance"));
    ASSERT_TRUE(hasService(publisher.services(), "healthy-observer", "healthy-instance"));

    failing_listener->failureController()->arm(0, "test.isolated-broadcast");
    ASSERT_TRUE(publisher.send(
        eventMessage("test.isolated-broadcast", "terminal-state")));

    EXPECT_EQ(failing_listener->failureController()->failureCount(), 1);
    EXPECT_EQ(healthy_deliveries.load(), 1);
    EXPECT_FALSE(hasService(publisher.services(), "slow-observer", "slow-instance"));
    EXPECT_TRUE(hasService(publisher.services(), "healthy-observer", "healthy-instance"));

    publisher.close();
    healthy.close();
    slow.close();
    hub.stop();
}

}  // namespace
