#include "topic_catalog_compiler.h"

#include "system.pb.h"
#include "topic_catalog.pb.h"

#include <google/protobuf/descriptor.h>
#include <gtest/gtest.h>

#include <string>

namespace {

namespace catalog = industrial_bus::catalog::v1;
namespace envelope = industrial_bus::protocol::v1;

catalog::TopicBinding* addEvent(catalog::TopicCatalog& input,
                                std::string symbol,
                                std::string topic,
                                std::string payload =
                                    "industrial_bus.system.v1.Error") {
    auto* binding = input.add_bindings();
    binding->set_symbol(std::move(symbol));
    binding->set_topic_pattern(std::move(topic));
    binding->set_message_type(envelope::EVENT);
    binding->set_payload_role(envelope::EVENT_FACT);
    binding->set_payload_type(std::move(payload));
    binding->set_schema_major(1);
    binding->set_max_payload_bytes(4096);
    return binding;
}

catalog::TopicCatalog validCatalog() {
    catalog::TopicCatalog input;
    input.set_catalog_version(1);
    addEvent(input, "kLiteral", "test.literal");
    auto* parameterized = addEvent(input, "kParameterized",
                                   "test.channel.{channel_uuid}.event");
    auto* parameter = parameterized->add_parameters();
    parameter->set_name("channel_uuid");
    parameter->set_constraint(catalog::UUID);
    return input;
}

const google::protobuf::DescriptorPool& descriptors() {
    (void)industrial_bus::system::v1::Error::descriptor();
    return *google::protobuf::DescriptorPool::generated_pool();
}

TEST(TopicCatalogGeneratorTest, CompilesLiteralAndParameterizedBindings) {
    const auto result =
        industrial_bus::catalog_tool::compileCatalog(validCatalog(), descriptors());
    ASSERT_TRUE(result.ok) << result.diagnostic;
    EXPECT_NE(result.public_header.find("kCatalogFingerprint"), std::string::npos);
    EXPECT_NE(result.runtime_table.find("test.literal"), std::string::npos);
    EXPECT_NE(result.runtime_table.find("{channel_uuid}"), std::string::npos);
    EXPECT_NE(result.fingerprint.find("fnv1a64:"), std::string::npos);
}

TEST(TopicCatalogGeneratorTest, RejectsDuplicateEffectiveKeys) {
    auto input = validCatalog();
    *input.add_bindings() = input.bindings(0);
    input.mutable_bindings(input.bindings_size() - 1)->set_symbol("kDuplicate");
    const auto result =
        industrial_bus::catalog_tool::compileCatalog(input, descriptors());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.diagnostic.find("duplicate"), std::string::npos);
    EXPECT_NE(result.diagnostic.find("test.literal"), std::string::npos);
}

TEST(TopicCatalogGeneratorTest, RejectsAmbiguousParameterizedPatterns) {
    auto input = validCatalog();
    auto* overlap = addEvent(input, "kOverlap", "test.channel.{other}.event");
    auto* parameter = overlap->add_parameters();
    parameter->set_name("other");
    parameter->set_constraint(catalog::UUID);
    const auto result =
        industrial_bus::catalog_tool::compileCatalog(input, descriptors());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.diagnostic.find("ambiguous"), std::string::npos);
    EXPECT_NE(result.diagnostic.find("kOverlap") == std::string::npos
                  ? result.diagnostic.find("{other}")
                  : result.diagnostic.find("kOverlap"),
              std::string::npos);
}

TEST(TopicCatalogGeneratorTest, RejectsUnsafeOrUndeclaredParameters) {
    auto input = validCatalog();
    input.mutable_bindings(1)->mutable_parameters(0)->set_name("wrong_name");
    const auto result =
        industrial_bus::catalog_tool::compileCatalog(input, descriptors());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.diagnostic.find("channel_uuid"), std::string::npos);
}

TEST(TopicCatalogGeneratorTest, RejectsMissingPayloadDescriptors) {
    auto input = validCatalog();
    input.mutable_bindings(0)->set_payload_type("missing.package.Payload");
    const auto result =
        industrial_bus::catalog_tool::compileCatalog(input, descriptors());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.diagnostic.find("test.literal"), std::string::npos);
    EXPECT_NE(result.diagnostic.find("missing.package.Payload"),
              std::string::npos);
}

TEST(TopicCatalogGeneratorTest, ProducesDeterministicOutputs) {
    const auto first =
        industrial_bus::catalog_tool::compileCatalog(validCatalog(), descriptors());
    const auto second =
        industrial_bus::catalog_tool::compileCatalog(validCatalog(), descriptors());
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(first.public_header, second.public_header);
    EXPECT_EQ(first.runtime_table, second.runtime_table);
    EXPECT_EQ(first.fingerprint, second.fingerprint);
}

TEST(TopicCatalogGeneratorTest, RequiresBothSuccessAndErrorReplies) {
    catalog::TopicCatalog input;
    input.set_catalog_version(1);
    auto* command = input.add_bindings();
    command->set_symbol("kCommand");
    command->set_topic_pattern("test.command");
    command->set_message_type(envelope::COMMAND);
    command->set_payload_role(envelope::COMMAND_INPUT);
    command->set_payload_type("industrial_bus.system.v1.Error");
    command->set_schema_major(1);
    command->set_max_payload_bytes(4096);
    const auto result =
        industrial_bus::catalog_tool::compileCatalog(input, descriptors());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.diagnostic.find("success and Error Reply"),
              std::string::npos);
}

}  // namespace
