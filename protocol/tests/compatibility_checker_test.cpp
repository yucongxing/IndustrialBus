#include "compatibility_checker.h"

#include "envelope.pb.h"
#include "topic_catalog.pb.h"

#include <google/protobuf/descriptor.pb.h>
#include <gtest/gtest.h>

#include <string>

namespace {

namespace descriptor = google::protobuf;
namespace catalog = industrial_bus::catalog::v1;
namespace envelope = industrial_bus::protocol::v1;

descriptor::FileDescriptorSet schema(
    std::string field_name = "value",
    descriptor::FieldDescriptorProto::Type field_type =
        descriptor::FieldDescriptorProto::TYPE_STRING,
    descriptor::FieldDescriptorProto::Label field_label =
        descriptor::FieldDescriptorProto::LABEL_OPTIONAL) {
    descriptor::FileDescriptorSet set;
    auto* file = set.add_file();
    file->set_name("test.proto");
    file->set_package("compat.v1");
    file->set_syntax("proto3");
    auto* message = file->add_message_type();
    message->set_name("Record");
    auto* field = message->add_field();
    field->set_name(std::move(field_name));
    field->set_number(1);
    field->set_type(field_type);
    field->set_label(field_label);
    auto* enum_type = file->add_enum_type();
    enum_type->set_name("State");
    auto* zero = enum_type->add_value();
    zero->set_name("STATE_UNSPECIFIED");
    zero->set_number(0);
    auto* one = enum_type->add_value();
    one->set_name("READY");
    one->set_number(1);
    return set;
}

catalog::TopicCatalog catalogSchema(std::string payload = "compat.v1.Record") {
    catalog::TopicCatalog result;
    result.set_catalog_version(1);
    auto* binding = result.add_bindings();
    binding->set_symbol("kRecord");
    binding->set_topic_pattern("compat.record.changed");
    binding->set_message_type(envelope::EVENT);
    binding->set_payload_role(envelope::EVENT_FACT);
    binding->set_payload_type(std::move(payload));
    binding->set_schema_major(1);
    binding->set_max_payload_bytes(1024);
    return result;
}

TEST(DescriptorCompatibilityTest, AcceptsAdditiveFieldsAndEnumValues) {
    const auto baseline = schema();
    auto current = schema();
    auto* added = current.mutable_file(0)->mutable_message_type(0)->add_field();
    added->set_name("added");
    added->set_number(2);
    added->set_type(descriptor::FieldDescriptorProto::TYPE_UINT64);
    added->set_label(descriptor::FieldDescriptorProto::LABEL_OPTIONAL);
    auto* enum_value = current.mutable_file(0)->mutable_enum_type(0)->add_value();
    enum_value->set_name("DONE");
    enum_value->set_number(2);
    const auto result = industrial_bus::compat::checkDescriptors(baseline, current);
    EXPECT_TRUE(result.compatible) << result.diagnostic;
}

TEST(DescriptorCompatibilityTest, RejectsFieldNumberReuse) {
    const auto result = industrial_bus::compat::checkDescriptors(
        schema(), schema("different_meaning"));
    EXPECT_FALSE(result.compatible);
    EXPECT_NE(result.diagnostic.find("compat.v1.Record field 1"),
              std::string::npos);
    EXPECT_NE(result.diagnostic.find("reused"), std::string::npos);
}

TEST(DescriptorCompatibilityTest, RejectsTypeAndCardinalityChanges) {
    auto changed_type = industrial_bus::compat::checkDescriptors(
        schema(), schema("value", descriptor::FieldDescriptorProto::TYPE_UINT64));
    EXPECT_FALSE(changed_type.compatible);
    EXPECT_NE(changed_type.diagnostic.find("field 1"), std::string::npos);

    auto changed_label = industrial_bus::compat::checkDescriptors(
        schema(), schema("value", descriptor::FieldDescriptorProto::TYPE_STRING,
                         descriptor::FieldDescriptorProto::LABEL_REPEATED));
    EXPECT_FALSE(changed_label.compatible);
    EXPECT_NE(changed_label.diagnostic.find("cardinality"), std::string::npos);
}

TEST(DescriptorCompatibilityTest, RequiresRemovedFieldNameAndNumberReservations) {
    const auto baseline = schema();
    auto current = schema();
    auto* message = current.mutable_file(0)->mutable_message_type(0);
    message->clear_field();
    auto missing = industrial_bus::compat::checkDescriptors(baseline, current);
    EXPECT_FALSE(missing.compatible);
    EXPECT_NE(missing.diagnostic.find("without reserving"), std::string::npos);

    auto* range = message->add_reserved_range();
    range->set_start(1);
    range->set_end(2);
    message->add_reserved_name("value");
    const auto reserved =
        industrial_bus::compat::checkDescriptors(baseline, current);
    EXPECT_TRUE(reserved.compatible) << reserved.diagnostic;
}

TEST(DescriptorCompatibilityTest, RejectsPublishedEnumRemovalOrRenumbering) {
    const auto baseline = schema();
    auto removed = schema();
    removed.mutable_file(0)->mutable_enum_type(0)->mutable_value()->RemoveLast();
    const auto removed_result =
        industrial_bus::compat::checkDescriptors(baseline, removed);
    EXPECT_FALSE(removed_result.compatible);
    EXPECT_NE(removed_result.diagnostic.find("READY"), std::string::npos);

    auto renumbered = schema();
    renumbered.mutable_file(0)->mutable_enum_type(0)->mutable_value(1)->set_number(9);
    const auto renumbered_result =
        industrial_bus::compat::checkDescriptors(baseline, renumbered);
    EXPECT_FALSE(renumbered_result.compatible);
    EXPECT_NE(renumbered_result.diagnostic.find("enum value 1"),
              std::string::npos);
}

TEST(CatalogCompatibilityTest, AcceptsAdditiveBindingsAndWiderLimits) {
    const auto baseline = catalogSchema();
    auto current = catalogSchema();
    current.mutable_bindings(0)->set_max_payload_bytes(2048);
    auto* added = current.add_bindings();
    *added = current.bindings(0);
    added->set_symbol("kAdded");
    added->set_topic_pattern("compat.record.added");
    const auto result = industrial_bus::compat::checkCatalog(baseline, current);
    EXPECT_TRUE(result.compatible) << result.diagnostic;
}

TEST(CatalogCompatibilityTest, RejectsRemovalAndInPlaceRebinding) {
    const auto baseline = catalogSchema();
    catalog::TopicCatalog empty;
    empty.set_catalog_version(1);
    auto removed = industrial_bus::compat::checkCatalog(baseline, empty);
    EXPECT_FALSE(removed.compatible);
    EXPECT_NE(removed.diagnostic.find("compat.record.changed"), std::string::npos);

    auto changed = catalogSchema("compat.v1.OtherRecord");
    auto rebound = industrial_bus::compat::checkCatalog(baseline, changed);
    EXPECT_FALSE(rebound.compatible);
    EXPECT_NE(rebound.diagnostic.find("old payload='compat.v1.Record'"),
              std::string::npos);
    EXPECT_NE(rebound.diagnostic.find("new payload='compat.v1.OtherRecord'"),
              std::string::npos);
}

}  // namespace
