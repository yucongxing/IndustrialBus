#include "industrial_bus/bus_client.h"
#include "industrial_bus/client/pending_request_table.h"
#include "industrial_bus/client/runtime_adapter.h"
#include "industrial_bus/core/idempotency_journal.h"
#include "industrial_bus/qos.h"

#include "operation.pb.h"
#include "system.pb.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace industrial_bus;

class ManualExecutor final : public IExecutor {
public:
    void post(std::function<void()> task) override {
        tasks.push_back(std::move(task));
    }
    void runAll() {
        auto pending = std::move(tasks);
        tasks.clear();
        for (auto& task : pending) {
            task();
        }
    }
    std::vector<std::function<void()>> tasks;
};

class FakeAdapter final : public client::IRuntimeAdapter {
public:
    Result<void> start() override {
        current_state = ConnectionState::Connected;
        if (connection_handler) {
            connection_handler(current_state, current_generation);
        }
        return {};
    }

    Result<void> send(client::OutboundMessage message) override {
        std::lock_guard<std::mutex> lock(mutex);
        sent.push_back(std::move(message));
        return {};
    }

    Result<void> subscribe(const std::string& topic) override {
        std::lock_guard<std::mutex> lock(mutex);
        subscribed_topics.push_back(topic);
        if (fail_subscribe) {
            return Error::resourceExhausted("injected subscribe failure");
        }
        return {};
    }

    Result<void> unsubscribe(const std::string& topic) override {
        std::lock_guard<std::mutex> lock(mutex);
        unsubscribed_topics.push_back(topic);
        if (fail_unsubscribe) {
            return Error::connectionLost("injected unsubscribe failure");
        }
        return {};
    }

    void setReceiveHandler(ReceiveHandler handler) override {
        receive_handler = std::move(handler);
    }
    void setConnectionHandler(ConnectionHandler handler) override {
        connection_handler = std::move(handler);
    }
    ConnectionState state() const override { return current_state; }
    std::uint64_t generation() const override { return current_generation; }
    ServiceSnapshot services() const override {
        ServiceInfo service;
        service.service_name = "worker";
        service.instance_id = "instance-1";
        service.node_id = "local";
        service.version = "0.1.0";
        service.local = true;
        return {service};
    }
    ServiceInfo localService() const override {
        ServiceInfo service;
        service.service_name = "test-client";
        service.instance_id = "test-instance";
        service.node_id = "local";
        service.version = "0.1.0";
        service.local = true;
        return service;
    }
    bool flush(std::chrono::milliseconds) override { return true; }
    void close() noexcept override { current_state = ConnectionState::Disconnected; }

    client::OutboundMessage lastSent() {
        std::lock_guard<std::mutex> lock(mutex);
        if (sent.empty()) {
            throw std::logic_error("no message was sent");
        }
        return sent.back();
    }

    std::size_t sentCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return sent.size();
    }

    std::size_t sentCount(MessageType message_type) const {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(
            std::count_if(sent.begin(), sent.end(),
                          [message_type](const client::OutboundMessage& message) {
                              return message.message_type == message_type;
                          }));
    }

    std::size_t subscribeCount(const std::string& topic) const {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(
            std::count(subscribed_topics.begin(), subscribed_topics.end(), topic));
    }

    std::size_t unsubscribeCount(const std::string& topic) const {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(
            std::count(unsubscribed_topics.begin(), unsubscribed_topics.end(), topic));
    }

    void deliver(client::InboundMessage message) {
        if (receive_handler) {
            receive_handler(std::move(message));
        }
    }

    void emitConnection(ConnectionState state, std::uint64_t generation) {
        current_state = state;
        current_generation = generation;
        if (connection_handler) {
            connection_handler(state, generation);
        }
    }

    void changeGeneration(std::uint64_t generation) {
        emitConnection(ConnectionState::Connected, generation);
    }

    mutable std::mutex mutex;
    std::vector<client::OutboundMessage> sent;
    std::vector<std::string> subscribed_topics;
    std::vector<std::string> unsubscribed_topics;
    ReceiveHandler receive_handler;
    ConnectionHandler connection_handler;
    ConnectionState current_state{ConnectionState::Disconnected};
    std::uint64_t current_generation{1};
    bool fail_subscribe{false};
    bool fail_unsubscribe{false};
};

TEST(PendingRequestTableTest, TimesOutAndCountsLateReply) {
    client::PendingRequestTable table;
    bool timed_out = false;
    EXPECT_TRUE(table.add("request-1", "reply.type",
                     client::PendingRequestTable::Clock::now() +
                         std::chrono::milliseconds(15),
                     [&](Result<ByteBuffer> result) {
                         timed_out = !result &&
                                     result.error().category == ErrorCategory::Timeout;
                     }));
    for (int i = 0; i < 50 && !timed_out; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(timed_out);
    EXPECT_TRUE(!table.complete("request-1", "reply.type", ByteBuffer{}));
    EXPECT_TRUE(table.lateReplyCount() == 1);
}

TEST(BusClientTest,
     RestoresSubscriptionsAndFailsPendingOperationsWithoutReplayOnGenerationChange) {
    auto adapter = std::make_shared<FakeAdapter>();
    ClientOptions options;
    options.service_name = "reconnect-policy-client";
    options.runtime_adapter = adapter;
    auto connected = BusClient::connect(options);
    ASSERT_TRUE(connected) << connected.error().message;
    auto bus = connected.value();

    const EventTopic<std::string> event_topic{"reconnect.event", "test.String"};
    auto subscription = bus->subscribe(event_topic, [](const std::string&) {});
    ASSERT_TRUE(subscription);
    ASSERT_EQ(adapter->subscribeCount("reconnect.event"), 1U);

    const RequestTopic<std::string, std::string> request_topic{
        "reconnect.request", "test.StringRequest", "test.StringReply"};
    auto request = bus->request(Destination{"worker", {}}, request_topic,
                                std::string("question"),
                                std::chrono::seconds(5));

    const CommandTopic<std::string, system::v1::CommandAck> command_topic{
        "reconnect.command", "test.StringCommand",
        "industrial_bus.system.v1.CommandAck"};
    CommandOptions command_options;
    command_options.idempotency_key = "reconnect-intent";
    command_options.accept_timeout = std::chrono::seconds(5);
    auto command = bus->command(Destination{"worker", {}}, command_topic,
                                std::string("run"), command_options);

    ASSERT_EQ(adapter->sentCount(MessageType::Request), 1U);
    ASSERT_EQ(adapter->sentCount(MessageType::Command), 1U);

    adapter->emitConnection(ConnectionState::Disconnected, 1);
    EXPECT_EQ(bus->connectionState(), ConnectionState::Disconnected);
    adapter->emitConnection(ConnectionState::Connected, 2);

    EXPECT_EQ(bus->connectionState(), ConnectionState::Connected);
    EXPECT_EQ(adapter->subscribeCount("reconnect.event"), 2U);
    EXPECT_EQ(adapter->sentCount(MessageType::Request), 1U);
    EXPECT_EQ(adapter->sentCount(MessageType::Command), 1U);

    ASSERT_EQ(request.wait_for(std::chrono::milliseconds(100)),
              std::future_status::ready);
    const auto request_result = request.get();
    ASSERT_FALSE(request_result);
    EXPECT_EQ(request_result.error().category, ErrorCategory::HubRestarted);

    ASSERT_EQ(command.wait_for(std::chrono::milliseconds(100)),
              std::future_status::ready);
    const auto command_result = command.get();
    ASSERT_FALSE(command_result);
    EXPECT_EQ(command_result.error().category, ErrorCategory::HubRestarted);

    EXPECT_TRUE(bus->shutdown(std::chrono::milliseconds(10)));
}

TEST(BusClientTest, PublishesDefaultAndExplicitSemanticClasses) {
    auto adapter = std::make_shared<FakeAdapter>();
    ClientOptions options;
    options.service_name = "semantic-publisher";
    options.runtime_adapter = adapter;
    auto connected = BusClient::connect(options);
    ASSERT_TRUE(connected) << connected.error().message;
    auto bus = connected.value();

    const EventTopic<std::string> event_topic{"semantic.event", "test.String"};
    ASSERT_TRUE(bus->publish(event_topic, std::string("event")));
    auto message = adapter->lastSent();
    EXPECT_EQ(message.message_type, MessageType::Event);
    EXPECT_EQ(message.priority, QosPriority::Normal);
    EXPECT_EQ(message.message_class, MessageClass::Event);

    const EventTopic<std::string> progress_topic{
        "semantic.progress", "test.String", MessageClass::Progress};
    PublishOptions progress_options;
    progress_options.operation_id = "operation-1";
    ASSERT_TRUE(bus->publish(progress_topic, std::string("progress"),
                             progress_options));
    message = adapter->lastSent();
    EXPECT_EQ(message.message_type, MessageType::Progress);
    EXPECT_EQ(message.priority, QosPriority::Normal);
    EXPECT_EQ(message.message_class, MessageClass::Progress);
    EXPECT_EQ(message.operation_id, "operation-1");

    const EventTopic<std::string> log_topic{
        "semantic.log", "test.String", MessageClass::WarnLog};
    ASSERT_TRUE(bus->publish(log_topic, std::string("warning")));
    message = adapter->lastSent();
    EXPECT_EQ(message.message_type, MessageType::Log);
    EXPECT_EQ(message.priority, QosPriority::Normal);
    EXPECT_EQ(message.message_class, MessageClass::WarnLog);

    PublishOptions record_override;
    record_override.message_class = MessageClass::InfoLog;
    ASSERT_TRUE(bus->publish(log_topic, std::string("info"), record_override));
    message = adapter->lastSent();
    EXPECT_EQ(message.message_type, MessageType::Log);
    EXPECT_EQ(message.priority, QosPriority::BestEffort);
    EXPECT_EQ(message.message_class, MessageClass::InfoLog);

    EXPECT_TRUE(bus->shutdown(std::chrono::milliseconds(10)));
}

TEST(BusClientTest, AssignsCanonicalPrioritiesToDirectedMessages) {
    auto adapter = std::make_shared<FakeAdapter>();
    ClientOptions options;
    options.service_name = "directed-priority-client";
    options.runtime_adapter = adapter;
    auto connected = BusClient::connect(options);
    ASSERT_TRUE(connected) << connected.error().message;
    auto bus = connected.value();

    const CommandTopic<std::string, system::v1::CommandAck> command_topic{
        "priority.command", "test.StringCommand",
        "industrial_bus.system.v1.CommandAck"};
    CommandOptions command_options;
    command_options.idempotency_key = "priority-command";
    auto command = bus->command(Destination{"worker", {}}, command_topic,
                                std::string("run"), command_options);
    auto message = adapter->lastSent();
    EXPECT_EQ(message.message_type, MessageType::Command);
    EXPECT_EQ(message.priority, QosPriority::High);
    EXPECT_EQ(message.message_class, MessageClass::Command);

    const RequestTopic<std::string, std::string> request_topic{
        "priority.request", "test.StringRequest", "test.StringReply"};
    auto request = bus->request(Destination{"worker", {}}, request_topic,
                                std::string("question"),
                                std::chrono::seconds(5));
    message = adapter->lastSent();
    EXPECT_EQ(message.message_type, MessageType::Request);
    EXPECT_EQ(message.priority, QosPriority::High);
    EXPECT_EQ(message.message_class, MessageClass::Request);

    auto executor = std::make_shared<ManualExecutor>();
    auto handler = bus->handleRequest(
        request_topic,
        [](const std::string&, const MessageContext&) -> Result<std::string> {
            return std::string("answer");
        },
        executor);
    ASSERT_TRUE(handler);
    client::InboundMessage incoming;
    incoming.message_type = MessageType::Request;
    incoming.priority = QosPriority::High;
    incoming.message_class = MessageClass::Request;
    incoming.message_id = "incoming-priority-request";
    incoming.source_service = "caller";
    incoming.source_instance = "caller-1";
    incoming.topic = "priority.request";
    incoming.payload_type = "test.StringRequest";
    ASSERT_TRUE(PayloadCodec<std::string>::encode("question", incoming.payload));
    adapter->deliver(std::move(incoming));
    executor->runAll();

    message = adapter->lastSent();
    EXPECT_EQ(message.message_type, MessageType::Reply);
    EXPECT_EQ(message.priority, QosPriority::Critical);
    EXPECT_EQ(message.message_class, MessageClass::Reply);

    EXPECT_TRUE(bus->shutdown(std::chrono::milliseconds(10)));
    EXPECT_EQ(command.wait_for(std::chrono::milliseconds(100)),
              std::future_status::ready);
    EXPECT_EQ(request.wait_for(std::chrono::milliseconds(100)),
              std::future_status::ready);
}

TEST(BusClientTest, ReportsOperationWithCanonicalPayloadType) {
    auto adapter = std::make_shared<FakeAdapter>();
    ClientOptions options;
    options.service_name = "operation-reporter";
    options.runtime_adapter = adapter;
    auto connected = BusClient::connect(options);
    ASSERT_TRUE(connected) << connected.error().message;
    auto bus = connected.value();

    OperationUpdate update;
    update.operation_id = "operation-1";
    update.operation_type = "test-operation";
    update.state = OperationState::Running;

    const auto reported = bus->reportOperation(std::move(update));
    ASSERT_TRUE(reported) << reported.error().message;
    auto message = adapter->lastSent();
    EXPECT_EQ(message.message_type, MessageType::Event);
    EXPECT_EQ(message.priority, QosPriority::Normal);
    EXPECT_EQ(message.message_class, MessageClass::Event);
    EXPECT_EQ(message.topic, "system.operation.event");
    EXPECT_EQ(message.payload_type,
              operation::v1::OperationEvent::descriptor()->full_name());

    OperationUpdate completed;
    completed.operation_id = "operation-1";
    completed.operation_type = "test-operation";
    completed.state = OperationState::Completed;
    ASSERT_TRUE(bus->reportOperation(std::move(completed)));
    message = adapter->lastSent();
    EXPECT_EQ(message.message_type, MessageType::Event);
    EXPECT_EQ(message.priority, QosPriority::Critical);
    EXPECT_EQ(message.message_class, MessageClass::FinalState);
    EXPECT_EQ(message.topic, "system.operation.event");

    OperationUpdate progress;
    progress.operation_id = "operation-1";
    progress.operation_type = "test-operation";
    progress.state = OperationState::Running;
    progress.stage = "calculating";
    progress.stage_progress = 0.25;
    progress.overall_progress = 0.5;
    progress.current = 3;
    progress.total = 12;
    progress.message = "three complete";
    ASSERT_TRUE(bus->reportProgress(std::move(progress), "progress-correlation"));
    message = adapter->lastSent();
    EXPECT_EQ(message.message_type, MessageType::Progress);
    EXPECT_EQ(message.priority, QosPriority::Normal);
    EXPECT_EQ(message.message_class, MessageClass::Progress);
    EXPECT_EQ(message.topic, "system.operation.progress");
    EXPECT_EQ(message.payload_type,
              operation::v1::ProgressEvent::descriptor()->full_name());
    EXPECT_EQ(message.operation_id, "operation-1");

    operation::v1::ProgressEvent wire_progress;
    ASSERT_TRUE(PayloadCodec<operation::v1::ProgressEvent>::decode(
        message.payload, wire_progress));
    EXPECT_EQ(wire_progress.operation_id(), message.operation_id);
    EXPECT_EQ(wire_progress.stage(), "calculating");
    EXPECT_DOUBLE_EQ(wire_progress.stage_progress(), 0.25);
    EXPECT_DOUBLE_EQ(wire_progress.overall_progress(), 0.5);
    EXPECT_EQ(wire_progress.current(), 3U);
    EXPECT_EQ(wire_progress.total(), 12U);
    EXPECT_EQ(wire_progress.message(), "three complete");

    EXPECT_TRUE(bus->shutdown(std::chrono::milliseconds(10)));
}

TEST(BusClientTest, SupportsLifecycleMessagingAndIdempotentHandlers) {
    char directory_template[] = "/tmp/industrial-bus-client-journal-XXXXXX";
    char* journal_directory = ::mkdtemp(directory_template);
    EXPECT_TRUE(journal_directory != nullptr);
    const std::string journal_path =
        std::string(journal_directory) + "/commands.journal";
    core::IdempotencyJournalOptions journal_options;
    journal_options.path = journal_path;
    auto opened_journal = core::IdempotencyJournal::open(journal_options);
    EXPECT_TRUE(opened_journal);

    auto adapter = std::make_shared<FakeAdapter>();
    ClientOptions options;
    options.service_name = "test-client";
    options.runtime_adapter = adapter;
    auto journal =
        std::shared_ptr<core::IdempotencyJournal>(std::move(opened_journal.value()));
    options.idempotency_journal = journal;
    auto connected = BusClient::connect(options);
    EXPECT_TRUE(connected);
    auto bus = connected.value();
    EXPECT_TRUE(bus->connectionState() == ConnectionState::Connected);

    auto second = BusClient::connect(options);
    EXPECT_TRUE(second && second.value() == bus);

    const EventTopic<std::string> event_topic{"test.event", "test.String"};
    auto executor = std::make_shared<ManualExecutor>();
    int callbacks = 0;
    auto subscription = bus->subscribe(
        event_topic, [&](const std::string&) { ++callbacks; }, executor);
    EXPECT_TRUE(adapter->subscribeCount("test.event") == 1);
    client::InboundMessage event;
    event.message_type = MessageType::Event;
    event.topic = "test.event";
    event.payload_type = "test.String";
    PayloadCodec<std::string>::encode("payload", event.payload);
    adapter->deliver(event);
    EXPECT_TRUE(executor->tasks.size() == 1);
    subscription.cancel();
    EXPECT_TRUE(adapter->unsubscribeCount("test.event") == 1);
    executor->runAll();
    EXPECT_TRUE(callbacks == 0);  // A queued callback must observe cancellation.

    auto active = bus->subscribe(
        event_topic, [&](const std::string& value) {
            EXPECT_TRUE(value == "payload");
            ++callbacks;
        }, executor);
    EXPECT_TRUE(adapter->subscribeCount("test.event") == 2);
    auto same_topic = bus->subscribe(
        event_topic, [&](const std::string&) { ++callbacks; }, executor);
    EXPECT_TRUE(same_topic);
    EXPECT_TRUE(adapter->subscribeCount("test.event") == 2);
    same_topic.cancel();
    EXPECT_TRUE(adapter->unsubscribeCount("test.event") == 1);
    adapter->deliver(event);
    executor->runAll();
    EXPECT_TRUE(callbacks == 1);
    Subscription moved = std::move(active);
    EXPECT_TRUE(!active && moved);

    adapter->fail_subscribe = true;
    const EventTopic<std::string> failed_topic{"test.failed", "test.String"};
    auto failed_subscription = bus->subscribe(
        failed_topic, [&](const std::string&) { ++callbacks; }, executor);
    EXPECT_TRUE(!failed_subscription);
    EXPECT_TRUE(adapter->subscribeCount("test.failed") == 1);
    adapter->fail_subscribe = false;
    auto retried_subscription = bus->subscribe(
        failed_topic, [&](const std::string&) { ++callbacks; }, executor);
    EXPECT_TRUE(retried_subscription);
    EXPECT_TRUE(adapter->subscribeCount("test.failed") == 2);
    adapter->fail_unsubscribe = true;
    retried_subscription.cancel();
    EXPECT_TRUE(adapter->unsubscribeCount("test.failed") == 1);
    adapter->fail_unsubscribe = false;

    const RequestTopic<std::string, std::string> request_topic{
        "test.request", "test.StringRequest", "test.StringReply"};
    auto response_future = bus->request(Destination{"worker", {}}, request_topic,
                                        std::string("question"),
                                        std::chrono::milliseconds(100));
    const auto request = adapter->lastSent();
    client::InboundMessage reply;
    reply.message_type = MessageType::Reply;
    reply.causation_id = request.message_id;
    reply.payload_type = "test.StringReply";
    PayloadCodec<std::string>::encode("answer", reply.payload);
    adapter->deliver(std::move(reply));
    auto response = response_future.get();
    EXPECT_TRUE(response && response.value() == "answer");

    auto timeout_future = bus->request(Destination{"worker", {}}, request_topic,
                                       std::string("slow"),
                                       std::chrono::milliseconds(20));
    auto timeout_result = timeout_future.get();
    EXPECT_TRUE(!timeout_result);
    EXPECT_TRUE(timeout_result.error().category == ErrorCategory::Timeout);

    auto restart_future = bus->request(Destination{"worker", {}}, request_topic,
                                       std::string("restart"),
                                       std::chrono::seconds(1));
    adapter->changeGeneration(2);
    EXPECT_TRUE(adapter->subscribeCount("test.event") == 3);
    auto restart_result = restart_future.get();
    EXPECT_TRUE(!restart_result);
    EXPECT_TRUE(restart_result.error().category == ErrorCategory::HubRestarted);

    const CommandTopic<std::string, system::v1::CommandAck> command_topic{
        "test.command", "test.StringCommand",
        "industrial_bus.system.v1.CommandAck"};
    auto invalid_command = bus->command(Destination{"worker", {}}, command_topic,
                                        std::string("run"));
    EXPECT_TRUE(invalid_command.get().error().category == ErrorCategory::InvalidArgument);

    CommandOptions command_options;
    command_options.idempotency_key = "intent-1";
    auto command_future = bus->command(Destination{"worker", {}}, command_topic,
                                       std::string("run"), command_options);
    const auto command = adapter->lastSent();
    client::InboundMessage ack;
    ack.message_type = MessageType::Reply;
    ack.causation_id = command.message_id;
    ack.payload_type = "industrial_bus.system.v1.CommandAck";
    system::v1::CommandAck wire_ack;
    wire_ack.set_operation_id("outgoing-operation");
    wire_ack.set_owner_service("worker");
    wire_ack.set_owner_instance("instance-1");
    PayloadCodec<system::v1::CommandAck>::encode(wire_ack, ack.payload);
    adapter->deliver(std::move(ack));
    auto accepted = command_future.get();
    EXPECT_TRUE(accepted && accepted.value().operation_id() == "outgoing-operation");

    bool request_handler_called = false;
    auto request_handler = bus->handleRequest(
        request_topic,
        [&](const std::string& value,
            const MessageContext& context) -> Result<std::string> {
            request_handler_called = true;
            EXPECT_TRUE(value == "incoming question");
            EXPECT_TRUE(context.message_id == "incoming-request-1");
            EXPECT_TRUE(context.correlation_id == "correlation-1");
            EXPECT_TRUE(context.operation_id == "operation-1");
            EXPECT_TRUE(context.source_service == "caller");
            EXPECT_TRUE(context.source_instance == "caller-1");
            EXPECT_TRUE(context.deadline_unix_ns == 12345);
            return std::string("incoming answer");
        },
        executor);
    EXPECT_TRUE(request_handler);

    client::InboundMessage incoming_request;
    incoming_request.message_type = MessageType::Request;
    incoming_request.message_id = "incoming-request-1";
    incoming_request.correlation_id = "correlation-1";
    incoming_request.operation_id = "operation-1";
    incoming_request.source_service = "caller";
    incoming_request.source_instance = "caller-1";
    incoming_request.topic = "test.request";
    incoming_request.payload_type = "test.StringRequest";
    incoming_request.deadline_unix_ns = 12345;
    PayloadCodec<std::string>::encode("incoming question", incoming_request.payload);
    adapter->deliver(incoming_request);
    EXPECT_TRUE(!request_handler_called);
    executor->runAll();
    EXPECT_TRUE(request_handler_called);
    const auto response_reply = adapter->lastSent();
    EXPECT_TRUE(response_reply.message_type == MessageType::Reply);
    EXPECT_TRUE(response_reply.payload_role == client::PayloadRole::ResponseData);
    EXPECT_TRUE(response_reply.causation_id == "incoming-request-1");
    EXPECT_TRUE(response_reply.correlation_id == "correlation-1");
    EXPECT_TRUE(response_reply.operation_id == "operation-1");
    EXPECT_TRUE(response_reply.destination.service_name == "caller");
    EXPECT_TRUE(response_reply.destination.instance_id == "caller-1");
    EXPECT_TRUE(response_reply.payload_type == "test.StringReply");
    std::string response_value;
    EXPECT_TRUE(PayloadCodec<std::string>::decode(response_reply.payload, response_value));
    EXPECT_TRUE(response_value == "incoming answer");

    int command_handler_calls = 0;
    auto command_handler = bus->handleCommand(
        command_topic,
        [&](const std::string& value, const MessageContext& context) -> Result<void> {
            ++command_handler_calls;
            EXPECT_TRUE(!context.operation_id.empty());
            if (value == "fail") {
                EXPECT_TRUE(context.idempotency_key == "incoming-intent-failed");
                return Error::internal("cannot enqueue command");
            }
            EXPECT_TRUE(context.idempotency_key == "incoming-intent-1");
            return {};
        },
        executor);
    EXPECT_TRUE(command_handler);
    client::InboundMessage incoming_command;
    incoming_command.message_type = MessageType::Command;
    incoming_command.message_id = "incoming-command-1";
    incoming_command.correlation_id = "correlation-2";
    incoming_command.source_service = "caller";
    incoming_command.source_instance = "caller-1";
    incoming_command.topic = "test.command";
    incoming_command.payload_type = "test.StringCommand";
    incoming_command.idempotency_key = "incoming-intent-1";
    PayloadCodec<std::string>::encode("run", incoming_command.payload);
    adapter->deliver(incoming_command);
    executor->runAll();
    EXPECT_TRUE(command_handler_calls == 1);
    const auto accepted_reply = adapter->lastSent();
    EXPECT_TRUE(accepted_reply.payload_role == client::PayloadRole::CommandAck);
    EXPECT_TRUE(accepted_reply.causation_id == "incoming-command-1");
    EXPECT_TRUE(accepted_reply.payload_type == "industrial_bus.system.v1.CommandAck");
    system::v1::CommandAck accepted_ack;
    EXPECT_TRUE(PayloadCodec<system::v1::CommandAck>::decode(accepted_reply.payload,
                                                        accepted_ack));
    EXPECT_TRUE(!accepted_ack.operation_id().empty());
    EXPECT_TRUE(accepted_ack.owner_service() == "test-client");
    EXPECT_TRUE(accepted_ack.owner_instance() == "test-instance");
    EXPECT_TRUE(!accepted_ack.duplicate());
    EXPECT_TRUE(accepted_ack.current_state() == operation::v1::RUNNING);

    incoming_command.message_id = "incoming-command-duplicate";
    adapter->deliver(incoming_command);
    executor->runAll();
    EXPECT_TRUE(command_handler_calls == 1);
    const auto duplicate_reply = adapter->lastSent();
    system::v1::CommandAck duplicate_ack;
    EXPECT_TRUE(PayloadCodec<system::v1::CommandAck>::decode(duplicate_reply.payload,
                                                        duplicate_ack));
    EXPECT_TRUE(duplicate_ack.duplicate());
    EXPECT_TRUE(duplicate_ack.operation_id() == accepted_ack.operation_id());

    incoming_command.message_id = "incoming-command-conflict";
    PayloadCodec<std::string>::encode("different", incoming_command.payload);
    adapter->deliver(incoming_command);
    executor->runAll();
    EXPECT_TRUE(command_handler_calls == 1);
    const auto conflict_reply = adapter->lastSent();
    EXPECT_TRUE(conflict_reply.error);
    EXPECT_TRUE(conflict_reply.error->category == ErrorCategory::IdempotencyConflict);

    incoming_command.message_id = "incoming-command-failed";
    incoming_command.idempotency_key = "incoming-intent-failed";
    PayloadCodec<std::string>::encode("fail", incoming_command.payload);
    adapter->deliver(incoming_command);
    executor->runAll();
    EXPECT_TRUE(command_handler_calls == 2);
    const auto failed_reply = adapter->lastSent();
    EXPECT_TRUE(failed_reply.error);
    EXPECT_TRUE(failed_reply.error->category == ErrorCategory::InternalError);
    incoming_command.message_id = "incoming-command-failed-retry";
    adapter->deliver(incoming_command);
    executor->runAll();
    EXPECT_TRUE(command_handler_calls == 2);
    const auto failed_retry_reply = adapter->lastSent();
    EXPECT_TRUE(failed_retry_reply.error);
    EXPECT_TRUE(failed_retry_reply.error->category == ErrorCategory::InternalError);

    const CommandTopic<system::v1::CommandAck, system::v1::CommandAck>
        protobuf_command_topic{"test.protobuf-command",
                               "industrial_bus.system.v1.CommandAck",
                               "industrial_bus.system.v1.CommandAck"};
    int malformed_handler_calls = 0;
    auto protobuf_command_handler = bus->handleCommand(
        protobuf_command_topic,
        [&](const system::v1::CommandAck&, const MessageContext&) -> Result<void> {
            ++malformed_handler_calls;
            return {};
        },
        executor);
    EXPECT_TRUE(protobuf_command_handler);
    const auto records_before_malformed = journal->liveRecordCount();
    client::InboundMessage malformed_command;
    malformed_command.message_type = MessageType::Command;
    malformed_command.message_id = "malformed-command";
    malformed_command.source_service = "caller";
    malformed_command.source_instance = "caller-1";
    malformed_command.topic = "test.protobuf-command";
    malformed_command.payload_type = "industrial_bus.system.v1.CommandAck";
    malformed_command.idempotency_key = "malformed-intent";
    malformed_command.payload = {0xffU};
    adapter->deliver(std::move(malformed_command));
    EXPECT_TRUE(malformed_handler_calls == 0);
    EXPECT_TRUE(journal->liveRecordCount() == records_before_malformed);
    const auto malformed_reply = adapter->lastSent();
    EXPECT_TRUE(malformed_reply.error);
    EXPECT_TRUE(malformed_reply.error->category == ErrorCategory::ProtocolError);

    const auto before_cancelled_delivery = adapter->sentCount();
    adapter->deliver(incoming_request);
    request_handler.cancel();
    executor->runAll();
    EXPECT_TRUE(adapter->sentCount() == before_cancelled_delivery);

    EXPECT_TRUE(bus->services().size() == 1);
    EXPECT_TRUE(bus->shutdown(std::chrono::milliseconds(10)));
    EXPECT_TRUE(adapter->unsubscribeCount("test.event") == 2);
    EXPECT_TRUE(bus->connectionState() == ConnectionState::Disconnected);
    EXPECT_TRUE(bus->shutdown(std::chrono::milliseconds(10)));
    EXPECT_TRUE(::unlink(journal_path.c_str()) == 0);
    EXPECT_TRUE(::rmdir(journal_directory) == 0);
}

}  // namespace
