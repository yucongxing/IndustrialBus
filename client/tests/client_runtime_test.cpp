#include "industrial_bus/bus_client.h"
#include "industrial_bus/client/pending_request_table.h"
#include "industrial_bus/client/runtime_adapter.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    bool flush(std::chrono::milliseconds) override { return true; }
    void close() noexcept override { current_state = ConnectionState::Disconnected; }

    client::OutboundMessage lastSent() {
        std::lock_guard<std::mutex> lock(mutex);
        assert(!sent.empty());
        return sent.back();
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
    auto adapter = std::make_shared<FakeAdapter>();
    ClientOptions options;
    options.service_name = "test-client";
    options.runtime_adapter = adapter;
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

    const CommandTopic<std::string, std::string> command_topic{
        "test.command", "test.StringCommand", "test.StringAck"};
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
    ack.payload_type = "test.StringAck";
    PayloadCodec<std::string>::encode("accepted", ack.payload);
    adapter->deliver(std::move(ack));
    auto accepted = command_future.get();
    assert(accepted && accepted.value() == "accepted");

    assert(bus->services().size() == 1);
    assert(bus->shutdown(std::chrono::milliseconds(10)));
    assert(bus->connectionState() == ConnectionState::Disconnected);
    assert(bus->shutdown(std::chrono::milliseconds(10)));
}

}  // namespace

int main() {
    testPendingTableTimeoutAndLateReply();
    testClientLifecycle();
    return 0;
}
