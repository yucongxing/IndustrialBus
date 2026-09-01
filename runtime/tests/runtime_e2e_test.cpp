#include "industrial_bus/bus_client.h"
#include "industrial_bus/core/idempotency_journal.h"
#include "industrial_bus/ids.h"
#include "industrial_bus/runtime/commhub_server.h"
#include "industrial_bus/runtime/envelope_codec.h"
#include "industrial_bus/runtime/transport_runtime_adapter.h"
#include "loopback_transport.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>

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

void testErrorEnvelopeRoundTrip() {
    client::OutboundMessage reply;
    reply.kind = client::MessageKind::Reply;
    reply.payload_role = client::PayloadRole::ErrorDetail;
    reply.message_id = "error-message";
    reply.correlation_id = "correlation";
    reply.causation_id = "request-message";
    reply.operation_id = "operation";
    reply.topic = "test.request";
    reply.payload_type = "industrial_bus.system.v1.Error";
    reply.error = std::make_shared<Error>(
        Error::idempotencyConflict("intent payload mismatch", 42));
    reply.error->operation_id = "operation";
    reply.error->context["key"] = "intent-1";

    auto encoded = runtime::encodeOutbound(
        reply, runtime::SourceIdentity{"node", "worker", "worker-instance"});
    assert(encoded);
    auto decoded = runtime::decodeInbound(encoded.value());
    assert(decoded);
    assert(decoded.value().kind == client::MessageKind::Reply);
    assert(decoded.value().payload_role == client::PayloadRole::ErrorDetail);
    assert(decoded.value().source_service == "worker");
    assert(decoded.value().source_instance == "worker-instance");
    assert(decoded.value().has_error);
    assert(decoded.value().error == *reply.error);
}

}  // namespace

int main() {
    char journal_directory_template[] = "/tmp/industrial-bus-e2e-journal-XXXXXX";
    char* journal_directory = ::mkdtemp(journal_directory_template);
    assert(journal_directory != nullptr);
    const std::string journal_path =
        std::string(journal_directory) + "/commands.journal";
    core::IdempotencyJournalOptions journal_options;
    journal_options.path = journal_path;
    auto opened_journal = core::IdempotencyJournal::open(journal_options);
    assert(opened_journal);

    testErrorEnvelopeRoundTrip();
    auto loopback = std::make_shared<transport::LoopbackHub>();
    const transport::Endpoint endpoint{"loopback://runtime-e2e"};
    runtime::CommHubServer hub(loopback, endpoint, "hub-e2e", "test-token");
    assert(hub.start());

    runtime::TransportRuntimeOptions rejected_options;
    rejected_options.endpoint = endpoint;
    rejected_options.connector = loopback;
    rejected_options.service_name = "rejected";
    rejected_options.instance_id = "rejected-instance";
    rejected_options.auth_token = "wrong-token";
    rejected_options.handshake_timeout = std::chrono::milliseconds(20);
    runtime::TransportRuntimeAdapter rejected(rejected_options);
    assert(!rejected.start());

    runtime::TransportRuntimeOptions caller_options;
    caller_options.endpoint = endpoint;
    caller_options.connector = loopback;
    caller_options.service_name = "caller";
    caller_options.instance_id = "caller-instance";
    caller_options.auth_token = "test-token";
    auto caller = std::make_shared<runtime::TransportRuntimeAdapter>(caller_options);

    runtime::TransportRuntimeOptions worker_options;
    worker_options.endpoint = endpoint;
    worker_options.connector = loopback;
    worker_options.service_name = "worker";
    worker_options.instance_id = "worker-instance";
    worker_options.auth_token = "test-token";
    auto worker = std::make_shared<runtime::TransportRuntimeAdapter>(worker_options);

    std::mutex mutex;
    std::condition_variable changed;
    std::string caller_event;
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
            assert(PayloadCodec<system::v1::CommandAck>::decode(message.payload, ack));
            command_ack = ack.operation_id();
            duplicate_ack = ack.duplicate();
            ++ack_count;
        } else {
            caller_event.assign(message.payload.begin(), message.payload.end());
        }
        changed.notify_all();
    });

    assert(caller->start());
    ClientOptions worker_client_options;
    worker_client_options.service_name = "worker";
    worker_client_options.runtime_adapter = worker;
    worker_client_options.idempotency_journal =
        std::shared_ptr<core::IdempotencyJournal>(std::move(opened_journal.value()));
    auto connected_worker = BusClient::connect(worker_client_options);
    assert(connected_worker);
    auto worker_bus = connected_worker.value();
    const CommandTopic<std::string, system::v1::CommandAck> command_topic{
        "test.command", "test.StringCommand",
        "industrial_bus.system.v1.CommandAck"};
    std::atomic<int> command_executions{0};
    auto command_handler = worker_bus->handleCommand(
        command_topic,
        [&](const std::string& value, const MessageContext&) -> Result<void> {
            assert(value == "execute");
            ++command_executions;
            return {};
        });
    assert(command_handler);

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
    const auto command_operation_id = generateUuidV4();
    command.operation_id = command_operation_id;
    command.destination = {"worker", {}};
    command.topic = "test.command";
    command.payload_type = "test.StringCommand";
    command.idempotency_key = "runtime-e2e-command";
    command.deadline_unix_ns = deadlineAfter(std::chrono::seconds(2));
    PayloadCodec<std::string>::encode("execute", command.payload);
    assert(caller->send(std::move(command)));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return command_ack == command_operation_id && ack_count == 1;
        }));
    }
    assert(command_executions == 1);
    assert(!duplicate_ack);

    client::OutboundMessage duplicate_command;
    duplicate_command.kind = client::MessageKind::Command;
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
    assert(caller->send(std::move(duplicate_command)));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return ack_count == 2 && duplicate_ack;
        }));
    }
    assert(command_executions == 1);

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

    assert(worker_bus->shutdown(std::chrono::milliseconds(20)));
    caller->close();
    hub.stop();
    assert(::unlink(journal_path.c_str()) == 0);
    assert(::rmdir(journal_directory) == 0);
    return 0;
}
