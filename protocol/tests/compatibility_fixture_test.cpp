#include "envelope.pb.h"
#include "operation.pb.h"

#include <google/protobuf/unknown_field_set.h>
#include <gtest/gtest.h>

#include <fstream>
#include <string>

namespace {

template <typename Message>
Message load(const std::string& name) {
    Message message;
    std::ifstream input(std::string(INDUSTRIAL_BUS_COMPAT_FIXTURE_DIR) + "/" + name,
                        std::ios::binary);
    EXPECT_TRUE(input) << name;
    EXPECT_TRUE(message.ParseFromIstream(&input)) << name;
    return message;
}

bool hasFixtureUnknownField(const google::protobuf::Message& message) {
    const auto& fields = message.GetReflection()->GetUnknownFields(message);
    for (int i = 0; i < fields.field_count(); ++i) {
        if (fields.field(i).number() == 250 &&
            fields.field(i).type() == google::protobuf::UnknownField::TYPE_VARINT &&
            fields.field(i).varint() == 7) {
            return true;
        }
    }
    return false;
}

TEST(ProtocolCompatibilityFixtureTest, DecodesHistoricalEnvelopeAndPayload) {
    const auto envelope =
        load<industrial_bus::protocol::v1::MessageEnvelope>("envelope_v1.bin");
    EXPECT_EQ(envelope.message_id(), "fixture-message-1");
    EXPECT_EQ(envelope.topic(), "system.operation.event");
    EXPECT_EQ(envelope.protocol_version(), 1U);
    EXPECT_EQ(envelope.schema_version(), 1U);
    EXPECT_TRUE(hasFixtureUnknownField(envelope));

    industrial_bus::operation::v1::OperationEvent nested;
    ASSERT_TRUE(nested.ParseFromString(envelope.payload()));
    EXPECT_EQ(nested.operation_id(), "fixture-operation-1");
    EXPECT_EQ(nested.operation_type(), "fixture.simulation");
    EXPECT_EQ(nested.state(), industrial_bus::operation::v1::RUNNING);
    EXPECT_TRUE(hasFixtureUnknownField(nested));

    const auto standalone =
        load<industrial_bus::operation::v1::OperationEvent>(
            "operation_event_v1.bin");
    EXPECT_EQ(standalone.stage(), "fixture-stage");
    EXPECT_DOUBLE_EQ(standalone.overall_progress(), 0.5);
}

TEST(ProtocolCompatibilityFixtureTest, PreservesUnknownFieldsOnRoundTrip) {
    auto envelope =
        load<industrial_bus::protocol::v1::MessageEnvelope>("envelope_v1.bin");
    std::string bytes;
    ASSERT_TRUE(envelope.SerializeToString(&bytes));
    industrial_bus::protocol::v1::MessageEnvelope reparsed;
    ASSERT_TRUE(reparsed.ParseFromString(bytes));
    EXPECT_TRUE(hasFixtureUnknownField(reparsed));

    industrial_bus::operation::v1::OperationEvent event;
    ASSERT_TRUE(event.ParseFromString(reparsed.payload()));
    ASSERT_TRUE(event.SerializeToString(&bytes));
    industrial_bus::operation::v1::OperationEvent event_reparsed;
    ASSERT_TRUE(event_reparsed.ParseFromString(bytes));
    EXPECT_TRUE(hasFixtureUnknownField(event_reparsed));
}

}  // namespace
