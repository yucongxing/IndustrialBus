#include "industrial_bus/ids.h"
#include "industrial_bus/runtime/commhub_server.h"
#include "industrial_bus/runtime/transport_runtime_adapter.h"
#include "loopback_transport.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace {

using namespace industrial_bus;

std::int64_t deadlineAfter(std::chrono::seconds duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               (std::chrono::system_clock::now() + duration).time_since_epoch())
        .count();
}
client::OutboundMessage eventMessage(std::string topic, std::string body) {
    client::OutboundMessage message;
    message.kind = client::MessageKind::Event;
    message.payload_role = client::PayloadRole::EventData;
    message.message_id = generateUuidV4();
    message.correlation_id = generateUuidV4();
    message.topic = std::move(topic);
    message.payload_type = "test.StringEvent";
    message.payload.assign(body.begin(), body.end());
    return message;
}

}  // namespace

int main() {
    auto loopback = std::make_shared<transport::LoopbackHub>();
    const transport::Endpoint endpoint{"loopback://runtime-e2e"};
    runtime::CommHubServer hub(loopback, endpoint, "hub-e2e");
    assert(hub.start());

    runtime::TransportRuntimeOptions caller_options;
    caller_options.endpoint = endpoint;
    caller_options.connector = loopback;
    caller_options.service_name = "caller";
    caller_options.instance_id = "caller-instance";
    auto caller = std::make_shared<runtime::TransportRuntimeAdapter>(caller_options);

    runtime::TransportRuntimeOptions worker_options;
    worker_options.endpoint = endpoint;
    worker_options.connector = loopback;
    worker_options.service_name = "worker";
    worker_options.instance_id = "worker-instance";
    auto worker = std::make_shared<runtime::TransportRuntimeAdapter>(worker_options);

    std::mutex mutex;
    std::condition_variable changed;
    std::string caller_event;
    std::string command_ack;
    Error route_error;

    caller->setReceiveHandler([&](client::InboundMessage message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (message.has_error) {
            route_error = std::move(message.error);
        } else if (message.payload_role == client::PayloadRole::CommandAck) {
            command_ack.assign(message.payload.begin(), message.payload.end());
        } else {
            caller_event.assign(message.payload.begin(), message.payload.end());
        }
        changed.notify_all();
    });

    worker->setReceiveHandler([&](client::InboundMessage message) {
        if (message.kind != client::MessageKind::Command) {
            return;
        }
        client::OutboundMessage reply;
        reply.kind = client::MessageKind::Reply;
        reply.payload_role = client::PayloadRole::CommandAck;
        reply.message_id = generateUuidV4();
        reply.correlation_id = message.correlation_id;
        reply.causation_id = message.message_id;
        reply.operation_id = message.operation_id;
        reply.topic = message.topic;
        reply.payload_type = "test.StringAck";
        const std::string accepted = "accepted";
        reply.payload.assign(accepted.begin(), accepted.end());
        assert(worker->send(std::move(reply)));
    });

    assert(caller->start());
    assert(worker->start());

    assert(caller->subscribe("test.event"));
    assert(worker->send(eventMessage("test.event", "broadcast")));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return caller_event == "broadcast";
        }));
    }

    client::OutboundMessage command;
    command.kind = client::MessageKind::Command;
    command.payload_role = client::PayloadRole::CommandData;
    command.message_id = generateUuidV4();
    command.correlation_id = generateUuidV4();
    command.operation_id = generateUuidV4();
    command.destination = {"worker", {}};
    command.topic = "test.command";
    command.payload_type = "test.StringCommand";
    command.idempotency_key = "runtime-e2e-command";
    command.deadline_unix_ns = deadlineAfter(std::chrono::seconds(2));
    command.payload = {1, 2, 3};
    assert(caller->send(std::move(command)));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return command_ack == "accepted";
        }));
    }

    client::OutboundMessage missing;
    missing.kind = client::MessageKind::Command;
    missing.payload_role = client::PayloadRole::CommandData;
    missing.message_id = generateUuidV4();
    missing.correlation_id = generateUuidV4();
    missing.destination = {"missing-service", {}};
    missing.topic = "test.missing";
    missing.payload_type = "test.MissingCommand";
    missing.idempotency_key = "runtime-e2e-missing";
    missing.deadline_unix_ns = deadlineAfter(std::chrono::seconds(2));
    assert(caller->send(std::move(missing)));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return route_error.category == ErrorCategory::ServiceUnavailable;
        }));
    }

    worker->close();
    caller->close();
    hub.stop();
    return 0;
}
