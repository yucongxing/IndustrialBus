#include "industrial_bus/runtime/commhub_server.h"
#include "industrial_bus/topic_catalog.h"
#include "loopback_transport.h"

#include "envelope.pb.h"
#include "system.pb.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace industrial_bus;
namespace wire = industrial_bus::protocol::v1;

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

transport::ByteBuffer serialize(const wire::MessageEnvelope& envelope) {
    std::string bytes;
    EXPECT_TRUE(envelope.SerializeToString(&bytes));
    return {bytes.begin(), bytes.end()};
}

wire::MessageEnvelope systemEnvelope(std::string topic,
                                     std::string payload_type,
                                     const google::protobuf::MessageLite& payload,
                                     std::string instance) {
    wire::MessageEnvelope envelope;
    envelope.set_message_id("message-" + instance);
    envelope.set_correlation_id("correlation-" + instance);
    envelope.set_source_node("node");
    envelope.set_source_service("service");
    envelope.set_source_instance(std::move(instance));
    envelope.set_topic(std::move(topic));
    envelope.set_message_type(wire::SYSTEM);
    envelope.set_timestamp_unix_ns(nowNs());
    envelope.set_protocol_version(1);
    envelope.set_schema_version(1);
    envelope.set_priority(1);
    envelope.set_max_hops(1);
    (*envelope.mutable_headers())["industrial_bus.message_class"] = "ServiceState";
    envelope.set_payload_type(std::move(payload_type));
    envelope.set_payload_role(wire::SYSTEM_PAYLOAD);
    payload.SerializeToString(envelope.mutable_payload());
    return envelope;
}

TopicCatalogView testCatalog() {
    static const std::vector<TopicBinding> bindings = [] {
        const auto production = productionTopicCatalog();
        std::vector<TopicBinding> result(production.begin(), production.end());
        result.push_back({"kTestRequest", "test.validation.request",
                          MessageType::Request,
                          CatalogPayloadRole::RequestInput, "test.Request", 1,
                          4096, ProtocolPayloadValidatorKind::None});
        result.push_back({"kTestResponse", "test.validation.request",
                          MessageType::Reply,
                          CatalogPayloadRole::ResponseOutput, "test.Response", 1,
                          4096, ProtocolPayloadValidatorKind::None});
        result.push_back({"kTestRequestError", "test.validation.request",
                          MessageType::Reply,
                          CatalogPayloadRole::ErrorDetail,
                          "industrial_bus.system.v1.Error", 1, 65536,
                          ProtocolPayloadValidatorKind::None});
        return result;
    }();
    return {bindings.data(), bindings.size()};
}

std::shared_ptr<transport::IConnection> connect(
    const std::shared_ptr<transport::LoopbackHub>& loopback,
    const transport::Endpoint& endpoint) {
    auto result = loopback->connect(endpoint).get();
    EXPECT_TRUE(result);
    return result ? std::move(result).value() : nullptr;
}

system::v1::ServiceRegistration registration(std::string instance) {
    system::v1::ServiceRegistration registration;
    registration.set_node_id("node");
    registration.set_service_name("service");
    registration.set_instance_id(std::move(instance));
    registration.set_version("1.0");
    return registration;
}

TEST(CommHubProtocolValidationTest, RejectsBeforeEveryControlOrRoutingSideEffect) {
    auto loopback = std::make_shared<transport::LoopbackHub>();
    const transport::Endpoint endpoint{"loopback://protocol-validation"};
    runtime::CommHubServerOptions options;
    options.topic_catalog = testCatalog();
    runtime::CommHubServer hub(loopback, endpoint, "validation-hub", {}, options);
    ASSERT_TRUE(hub.start());

    auto invalid_peer = connect(loopback, endpoint);
    ASSERT_NE(invalid_peer, nullptr);
    auto invalid_registration = systemEnvelope(
        "system.service.register",
        "industrial_bus.system.v1.ServiceRegistration",
        registration("invalid-instance"), "invalid-instance");
    (*invalid_registration.mutable_headers())["industrial_bus.unregistered"] = "x";
    ASSERT_TRUE(invalid_peer->send(transport::ByteView(serialize(invalid_registration))));
    EXPECT_TRUE(hub.core().sessions().snapshot().empty());

    auto peer = connect(loopback, endpoint);
    ASSERT_NE(peer, nullptr);
    auto valid_registration = systemEnvelope(
        "system.service.register",
        "industrial_bus.system.v1.ServiceRegistration",
        registration("valid-instance"), "valid-instance");
    ASSERT_TRUE(peer->send(transport::ByteView(serialize(valid_registration))));
    auto session = hub.core().sessions().find("valid-instance");
    ASSERT_TRUE(session.has_value());

    system::v1::SubscriptionUpdate update;
    update.set_topic("test.validation.request");
    update.set_subscribe(true);
    auto invalid_subscription = systemEnvelope(
        "system.subscription.update",
        "industrial_bus.system.v1.SubscriptionUpdate", update, "valid-instance");
    invalid_subscription.set_schema_version(2);
    ASSERT_TRUE(peer->send(transport::ByteView(serialize(invalid_subscription))));
    EXPECT_TRUE(hub.core().sessions()
                    .subscribers("test.validation.request")
                    .empty());

    const auto before_heartbeat =
        hub.core().sessions().find("valid-instance")->last_heartbeat;
    system::v1::Heartbeat heartbeat;
    heartbeat.set_sequence(1);
    heartbeat.set_sent_at_unix_ns(nowNs());
    auto invalid_heartbeat = systemEnvelope(
        "system.heartbeat", "industrial_bus.system.v1.Heartbeat", heartbeat,
        "valid-instance");
    invalid_heartbeat.set_message_type(wire::HEARTBEAT);
    (*invalid_heartbeat.mutable_headers())["industrial_bus.message_class"] =
        "Heartbeat";
    invalid_heartbeat.set_timestamp_unix_ns(
        nowNs() + std::chrono::hours(1).count() * 60LL * 60LL *
                      1'000'000'000LL / std::chrono::hours(1).count());
    ASSERT_TRUE(peer->send(transport::ByteView(serialize(invalid_heartbeat))));
    EXPECT_EQ(hub.core().sessions().find("valid-instance")->last_heartbeat,
              before_heartbeat);

    system::v1::ServiceDisconnected disconnected;
    disconnected.set_node_id("node");
    disconnected.set_service_name("service");
    disconnected.set_instance_id("valid-instance");
    auto invalid_disconnect = systemEnvelope(
        "system.service.disconnect",
        "industrial_bus.system.v1.ServiceDisconnected", disconnected,
        "valid-instance");
    invalid_disconnect.set_protocol_version(99);
    ASSERT_TRUE(peer->send(transport::ByteView(serialize(invalid_disconnect))));
    EXPECT_TRUE(hub.core().sessions().find("valid-instance").has_value());

    std::vector<transport::ByteBuffer> replies;
    peer->setReceiveHandler(
        [&](transport::ByteBuffer reply) { replies.push_back(std::move(reply)); });
    wire::MessageEnvelope invalid_request;
    invalid_request.set_message_id("invalid-request");
    invalid_request.set_correlation_id("request-correlation");
    invalid_request.set_source_node("node");
    invalid_request.set_source_service("service");
    invalid_request.set_source_instance("valid-instance");
    invalid_request.set_destination_service("missing-service");
    invalid_request.set_topic("test.validation.request");
    invalid_request.set_message_type(wire::REQUEST);
    invalid_request.set_timestamp_unix_ns(nowNs());
    invalid_request.set_deadline_unix_ns(nowNs() + 60'000'000'000LL);
    invalid_request.set_protocol_version(1);
    invalid_request.set_schema_version(1);
    invalid_request.set_priority(1);
    invalid_request.set_max_hops(1);
    (*invalid_request.mutable_headers())["industrial_bus.message_class"] = "Request";
    invalid_request.set_payload_type("test.WrongRequest");
    invalid_request.set_payload_role(wire::REQUEST_INPUT);
    invalid_request.set_payload("opaque");
    ASSERT_TRUE(peer->send(transport::ByteView(serialize(invalid_request))));
    EXPECT_EQ(hub.core().returnRoutes().size(), 0U);
    EXPECT_EQ(hub.core().qos().stats("valid-instance").enqueued, 0U);
    ASSERT_FALSE(replies.empty());
    wire::MessageEnvelope error_reply;
    ASSERT_TRUE(error_reply.ParseFromArray(replies.back().data(),
                                           static_cast<int>(replies.back().size())));
    EXPECT_EQ(error_reply.message_type(), wire::REPLY);
    EXPECT_EQ(error_reply.payload_role(), wire::ERROR_DETAIL);
    system::v1::Error detail;
    ASSERT_TRUE(detail.ParseFromString(error_reply.payload()));
    EXPECT_EQ(detail.category(), system::v1::PROTOCOL_ERROR);

    peer->close();
    invalid_peer->close();
    hub.stop();
}

}  // namespace
