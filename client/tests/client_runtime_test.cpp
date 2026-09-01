#include "industrial_bus/bus_client.h"
#include "industrial_bus/client/pending_request_table.h"
#include "industrial_bus/client/runtime_adapter.h"
#include "industrial_bus/core/idempotency_journal.h"

#include "system.pb.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

    void setReceiveHandler(ReceiveHandler handler) override {
        receive_handler = std::move(handler);
    }
    void setConnectionHandler(ConnectionHandler handler) override {
        connection_handler = std::move(handler);
    }
    ConnectionState state() const override { return current_state; }
    std::uint64_t generation() const override { return current_generation; }
    ServiceSnapshot services() const override { return {{"worker", "instance-1", true}}; }
    ServiceInfo localService() const override {
        return {"test-client", "test-instance", true};
    }
    bool flush(std::chrono::milliseconds) override { return true; }
    void close() noexcept override { current_state = ConnectionState::Disconnected; }

    client::OutboundMessage lastSent() {
        std::lock_guard<std::mutex> lock(mutex);
        assert(!sent.empty());
        return sent.back();
    }

    std::size_t sentCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return sent.size();
    }

    void deliver(client::InboundMessage message) {
        if (receive_handler) {
            receive_handler(std::move(message));
        }
    }

    void changeGeneration(std::uint64_t generation) {
        current_generation = generation;
        if (connection_handler) {
            connection_handler(ConnectionState::Connected, generation);
        }
    }

    mutable std::mutex mutex;
    std::vector<client::OutboundMessage> sent;
    ReceiveHandler receive_handler;
    ConnectionHandler connection_handler;
    ConnectionState current_state{ConnectionState::Disconnected};
    std::uint64_t current_generation{1};
};

void testPendingTableTimeoutAndLateReply() {
    client::PendingRequestTable table;
    bool timed_out = false;
    assert(table.add("request-1", "reply.type",
                     client::PendingRequestTable::Clock::now() +
                         std::chrono::milliseconds(15),
                     [&](Result<ByteBuffer> result) {
                         timed_out = !result &&
                                     result.error().category == ErrorCategory::Timeout;
                     }));
    for (int i = 0; i < 50 && !timed_out; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(timed_out);
    assert(!table.complete("request-1", "reply.type", ByteBuffer{}));
    assert(table.lateReplyCount() == 1);
}

void testClientLifecycle() {
    char directory_template[] = "/tmp/industrial-bus-client-journal-XXXXXX";
    char* journal_directory = ::mkdtemp(directory_template);
    assert(journal_directory != nullptr);
    const std::string journal_path =
        std::string(journal_directory) + "/commands.journal";
    core::IdempotencyJournalOptions journal_options;
    journal_options.path = journal_path;
    auto opened_journal = core::IdempotencyJournal::open(journal_options);
    assert(opened_journal);

    auto adapter = std::make_shared<FakeAdapter>();
    ClientOptions options;
    options.service_name = "test-client";
    options.runtime_adapter = adapter;
    auto journal =
        std::shared_ptr<core::IdempotencyJournal>(std::move(opened_journal.value()));
    options.idempotency_journal = journal;
    auto connected = BusClient::connect(options);
    assert(connected);
    auto bus = connected.value();
    assert(bus->connectionState() == ConnectionState::Connected);

    auto second = BusClient::connect(options);
    assert(second && second.value() == bus);

    const EventTopic<std::string> event_topic{"test.event", "test.String"};
    auto executor = std::make_shared<ManualExecutor>();
    int callbacks = 0;
    auto subscription = bus->subscribe(
        event_topic, [&](const std::string&) { ++callbacks; }, executor);
    client::InboundMessage event;
    event.kind = client::MessageKind::Event;
    event.topic = "test.event";
    event.payload_type = "test.String";
    PayloadCodec<std::string>::encode("payload", event.payload);
    adapter->deliver(event);
    assert(executor->tasks.size() == 1);
    subscription.cancel();
    executor->runAll();
    assert(callbacks == 0);  // A queued callback must observe cancellation.

    auto active = bus->subscribe(
        event_topic, [&](const std::string& value) {
            assert(value == "payload");
            ++callbacks;
        }, executor);
    adapter->deliver(event);
    executor->runAll();
    assert(callbacks == 1);
    Subscription moved = std::move(active);
    assert(!active && moved);

    const RequestTopic<std::string, std::string> request_topic{
        "test.request", "test.StringRequest", "test.StringReply"};
    auto response_future = bus->request(Destination{"worker", {}}, request_topic,
                                        std::string("question"),
                                        std::chrono::milliseconds(100));
    const auto request = adapter->lastSent();
    client::InboundMessage reply;
    reply.kind = client::MessageKind::Reply;
    reply.causation_id = request.message_id;
    reply.payload_type = "test.StringReply";
    PayloadCodec<std::string>::encode("answer", reply.payload);
    adapter->deliver(std::move(reply));
    auto response = response_future.get();
    assert(response && response.value() == "answer");

    auto timeout_future = bus->request(Destination{"worker", {}}, request_topic,
                                       std::string("slow"),
                                       std::chrono::milliseconds(20));
    auto timeout_result = timeout_future.get();
    assert(!timeout_result);
    assert(timeout_result.error().category == ErrorCategory::Timeout);

    auto restart_future = bus->request(Destination{"worker", {}}, request_topic,
                                       std::string("restart"),
                                       std::chrono::seconds(1));
    adapter->changeGeneration(2);
    auto restart_result = restart_future.get();
    assert(!restart_result);
    assert(restart_result.error().category == ErrorCategory::HubRestarted);

    const CommandTopic<std::string, system::v1::CommandAck> command_topic{
        "test.command", "test.StringCommand",
        "industrial_bus.system.v1.CommandAck"};
    auto invalid_command = bus->command(Destination{"worker", {}}, command_topic,
                                        std::string("run"));
    assert(invalid_command.get().error().category == ErrorCategory::InvalidArgument);

    CommandOptions command_options;
    command_options.idempotency_key = "intent-1";
    auto command_future = bus->command(Destination{"worker", {}}, command_topic,
                                       std::string("run"), command_options);
    const auto command = adapter->lastSent();
    client::InboundMessage ack;
    ack.kind = client::MessageKind::Reply;
    ack.causation_id = command.message_id;
    ack.payload_type = "industrial_bus.system.v1.CommandAck";
    system::v1::CommandAck wire_ack;
    wire_ack.set_operation_id("outgoing-operation");
    wire_ack.set_owner_service("worker");
    wire_ack.set_owner_instance("instance-1");
    PayloadCodec<system::v1::CommandAck>::encode(wire_ack, ack.payload);
    adapter->deliver(std::move(ack));
    auto accepted = command_future.get();
    assert(accepted && accepted.value().operation_id() == "outgoing-operation");

    bool request_handler_called = false;
    auto request_handler = bus->handleRequest(
        request_topic,
        [&](const std::string& value,
            const MessageContext& context) -> Result<std::string> {
            request_handler_called = true;
            assert(value == "incoming question");
            assert(context.message_id == "incoming-request-1");
            assert(context.correlation_id == "correlation-1");
            assert(context.operation_id == "operation-1");
            assert(context.source_service == "caller");
            assert(context.source_instance == "caller-1");
            assert(context.deadline_unix_ns == 12345);
            return std::string("incoming answer");
        },
        executor);
    assert(request_handler);

    client::InboundMessage incoming_request;
    incoming_request.kind = client::MessageKind::Request;
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
    assert(!request_handler_called);
    executor->runAll();
    assert(request_handler_called);
    const auto response_reply = adapter->lastSent();
    assert(response_reply.kind == client::MessageKind::Reply);
    assert(response_reply.payload_role == client::PayloadRole::ResponseData);
    assert(response_reply.causation_id == "incoming-request-1");
    assert(response_reply.correlation_id == "correlation-1");
    assert(response_reply.operation_id == "operation-1");
    assert(response_reply.destination.service_name == "caller");
    assert(response_reply.destination.instance_id == "caller-1");
    assert(response_reply.payload_type == "test.StringReply");
    std::string response_value;
    assert(PayloadCodec<std::string>::decode(response_reply.payload, response_value));
    assert(response_value == "incoming answer");

    int command_handler_calls = 0;
    auto command_handler = bus->handleCommand(
        command_topic,
        [&](const std::string& value, const MessageContext& context) -> Result<void> {
            ++command_handler_calls;
            assert(!context.operation_id.empty());
            if (value == "fail") {
                assert(context.idempotency_key == "incoming-intent-failed");
                return Error::internal("cannot enqueue command");
            }
            assert(context.idempotency_key == "incoming-intent-1");
            return {};
        },
        executor);
    assert(command_handler);
    client::InboundMessage incoming_command;
    incoming_command.kind = client::MessageKind::Command;
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
    assert(command_handler_calls == 1);
    const auto accepted_reply = adapter->lastSent();
    assert(accepted_reply.payload_role == client::PayloadRole::CommandAck);
    assert(accepted_reply.causation_id == "incoming-command-1");
    assert(accepted_reply.payload_type == "industrial_bus.system.v1.CommandAck");
    system::v1::CommandAck accepted_ack;
    assert(PayloadCodec<system::v1::CommandAck>::decode(accepted_reply.payload,
                                                        accepted_ack));
    assert(!accepted_ack.operation_id().empty());
    assert(accepted_ack.owner_service() == "test-client");
    assert(accepted_ack.owner_instance() == "test-instance");
    assert(!accepted_ack.duplicate());
    assert(accepted_ack.current_state() == operation::v1::RUNNING);

    incoming_command.message_id = "incoming-command-duplicate";
    adapter->deliver(incoming_command);
    executor->runAll();
    assert(command_handler_calls == 1);
    const auto duplicate_reply = adapter->lastSent();
    system::v1::CommandAck duplicate_ack;
    assert(PayloadCodec<system::v1::CommandAck>::decode(duplicate_reply.payload,
                                                        duplicate_ack));
    assert(duplicate_ack.duplicate());
    assert(duplicate_ack.operation_id() == accepted_ack.operation_id());

    incoming_command.message_id = "incoming-command-conflict";
    PayloadCodec<std::string>::encode("different", incoming_command.payload);
    adapter->deliver(incoming_command);
    executor->runAll();
    assert(command_handler_calls == 1);
    const auto conflict_reply = adapter->lastSent();
    assert(conflict_reply.error);
    assert(conflict_reply.error->category == ErrorCategory::IdempotencyConflict);

    incoming_command.message_id = "incoming-command-failed";
    incoming_command.idempotency_key = "incoming-intent-failed";
    PayloadCodec<std::string>::encode("fail", incoming_command.payload);
    adapter->deliver(incoming_command);
    executor->runAll();
    assert(command_handler_calls == 2);
    const auto failed_reply = adapter->lastSent();
    assert(failed_reply.error);
    assert(failed_reply.error->category == ErrorCategory::InternalError);
    incoming_command.message_id = "incoming-command-failed-retry";
    adapter->deliver(incoming_command);
    executor->runAll();
    assert(command_handler_calls == 2);
    const auto failed_retry_reply = adapter->lastSent();
    assert(failed_retry_reply.error);
    assert(failed_retry_reply.error->category == ErrorCategory::InternalError);

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
    assert(protobuf_command_handler);
    const auto records_before_malformed = journal->liveRecordCount();
    client::InboundMessage malformed_command;
    malformed_command.kind = client::MessageKind::Command;
    malformed_command.message_id = "malformed-command";
    malformed_command.source_service = "caller";
    malformed_command.source_instance = "caller-1";
    malformed_command.topic = "test.protobuf-command";
    malformed_command.payload_type = "industrial_bus.system.v1.CommandAck";
    malformed_command.idempotency_key = "malformed-intent";
    malformed_command.payload = {0xffU};
    adapter->deliver(std::move(malformed_command));
    assert(malformed_handler_calls == 0);
    assert(journal->liveRecordCount() == records_before_malformed);
    const auto malformed_reply = adapter->lastSent();
    assert(malformed_reply.error);
    assert(malformed_reply.error->category == ErrorCategory::ProtocolError);

    const auto before_cancelled_delivery = adapter->sentCount();
    adapter->deliver(incoming_request);
    request_handler.cancel();
    executor->runAll();
    assert(adapter->sentCount() == before_cancelled_delivery);

    assert(bus->services().size() == 1);
    assert(bus->shutdown(std::chrono::milliseconds(10)));
    assert(bus->connectionState() == ConnectionState::Disconnected);
    assert(bus->shutdown(std::chrono::milliseconds(10)));
    assert(::unlink(journal_path.c_str()) == 0);
    assert(::rmdir(journal_directory) == 0);
}

}  // namespace

int main() {
    testPendingTableTimeoutAndLateReply();
    testClientLifecycle();
    return 0;
}
