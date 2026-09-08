#include "industrial_bus/topic_catalog.h"
#include "industrial_bus/topics.generated.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {

using namespace industrial_bus;

TEST(TopicCatalogTest, ResolvesEveryProductionBindingUniquely) {
    const auto catalog = productionTopicCatalog();
    EXPECT_EQ(catalog.size(), 20U);
    EXPECT_FALSE(productionTopicCatalogFingerprint().empty());
    for (const auto& binding : catalog) {
        SCOPED_TRACE(std::string(binding.symbol));
        std::string topic(binding.topic_pattern);
        const auto placeholder = topic.find("{channel_uuid}");
        if (placeholder != std::string::npos) {
            topic.replace(placeholder, std::string("{channel_uuid}").size(),
                          "123e4567-e89b-12d3-a456-426614174000");
        }
        const auto* resolved =
            catalog.find(topic, binding.message_type, binding.payload_role);
        ASSERT_NE(resolved, nullptr);
        EXPECT_EQ(resolved->payload_type, binding.payload_type);
        EXPECT_EQ(resolved->schema_major, binding.schema_major);
        EXPECT_EQ(resolved->max_payload_bytes, binding.max_payload_bytes);
    }
}

TEST(TopicCatalogTest, UsesLiteralPrecedenceBeforeParameterizedPatterns) {
    const TopicBinding bindings[] = {
        {"kPattern", "cmp.channel.{channel_uuid}.simulation.run",
         MessageType::Command, CatalogPayloadRole::CommandInput, "test.Pattern", 1,
         10, ProtocolPayloadValidatorKind::None},
        {"kLiteral", "cmp.channel.123e4567-e89b-12d3-a456-426614174000.simulation.run",
         MessageType::Command, CatalogPayloadRole::CommandInput, "test.Literal", 1,
         20, ProtocolPayloadValidatorKind::None},
    };
    const TopicCatalogView catalog(bindings, 2);
    const auto* resolved = catalog.find(
        "cmp.channel.123e4567-e89b-12d3-a456-426614174000.simulation.run",
        MessageType::Command, CatalogPayloadRole::CommandInput);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->symbol, "kLiteral");
}

TEST(TopicCatalogTest, RejectsUnsafeParameterizedSegments) {
    const auto catalog = productionTopicCatalog();
    EXPECT_TRUE(catalog.containsTopic(
        "cmp.channel.123e4567-e89b-12d3-a456-426614174000.simulation.run"));
    EXPECT_FALSE(catalog.containsTopic("cmp.channel..simulation.run"));
    EXPECT_FALSE(catalog.containsTopic("cmp.channel.not-a-uuid.simulation.run"));
    EXPECT_FALSE(catalog.containsTopic(
        "cmp.channel.123e4567-e89b-12d3-a456-426614174000.extra.simulation.run"));
    EXPECT_FALSE(isValidTopicParameter("bad.value", TopicParameterConstraint::Uuid));
    EXPECT_FALSE(isValidTopicParameter("*", TopicParameterConstraint::Uuid));
}

TEST(TopicCatalogTest, GeneratedChannelFactoryOwnsAndValidatesItsTopics) {
    const std::string channel = "123e4567-e89b-12d3-a456-426614174000";
    topics::ChannelTopics generated(channel);
    EXPECT_EQ(generated.runSimulation().name,
              "cmp.channel." + channel + ".simulation.run");
    EXPECT_EQ(generated.applicationLog().name, "application.log." + channel);
    EXPECT_THROW(topics::ChannelTopics("not-a-uuid"), std::invalid_argument);
    EXPECT_THROW(topics::ChannelTopics(""), std::invalid_argument);
}

TEST(TopicCatalogTest, KeepsExistingTypedTopicConstructionSourceCompatible) {
    const EventTopic<std::string> event{"custom.topic", "custom.Payload"};
    const CommandTopic<std::string, std::string> command{
        "custom.command", "custom.Command", "custom.Ack"};
    const RequestTopic<std::string, std::string> request{
        "custom.request", "custom.Request", "custom.Response"};
    EXPECT_EQ(event.name, "custom.topic");
    EXPECT_EQ(command.command_type, "custom.Command");
    EXPECT_EQ(request.response_type, "custom.Response");
    EXPECT_FALSE(productionTopicCatalog().containsTopic(event.name));
}

}  // namespace
