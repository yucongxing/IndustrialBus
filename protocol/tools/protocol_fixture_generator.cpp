#include "envelope.pb.h"
#include "operation.pb.h"

#include <google/protobuf/unknown_field_set.h>

#include <fstream>
#include <iostream>
#include <string>

namespace {

bool write(const std::string& path, const google::protobuf::Message& message) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    return output && message.SerializeToOstream(&output);
}

void addUnknownField(google::protobuf::Message& message) {
    message.GetReflection()->MutableUnknownFields(&message)->AddVarint(250, 7);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: protocol_fixture_generator OUTPUT_DIRECTORY\n";
        return 2;
    }
    industrial_bus::operation::v1::OperationEvent event;
    event.set_operation_id("fixture-operation-1");
    event.set_operation_type("fixture.simulation");
    event.set_state(industrial_bus::operation::v1::RUNNING);
    event.set_stage("fixture-stage");
    event.set_overall_progress(0.5);
    addUnknownField(event);

    industrial_bus::protocol::v1::MessageEnvelope envelope;
    envelope.set_message_id("fixture-message-1");
    envelope.set_correlation_id("fixture-correlation-1");
    envelope.set_source_node("fixture-node");
    envelope.set_source_service("fixture-service");
    envelope.set_source_instance("fixture-instance");
    envelope.set_topic("system.operation.event");
    envelope.set_message_type(industrial_bus::protocol::v1::EVENT);
    envelope.set_timestamp_unix_ns(1'700'000'000'000'000'000LL);
    envelope.set_protocol_version(1);
    envelope.set_schema_version(1);
    envelope.set_priority(2);
    envelope.set_max_hops(1);
    (*envelope.mutable_headers())["industrial_bus.message_class"] = "Event";
    envelope.set_operation_id(event.operation_id());
    envelope.set_payload_type("industrial_bus.operation.v1.OperationEvent");
    envelope.set_payload_role(industrial_bus::protocol::v1::EVENT_FACT);
    event.SerializeToString(envelope.mutable_payload());
    addUnknownField(envelope);

    const std::string directory = argv[1];
    if (!write(directory + "/operation_event_v1.bin", event) ||
        !write(directory + "/envelope_v1.bin", envelope)) {
        std::cerr << "failed to write compatibility fixtures\n";
        return 1;
    }
    return 0;
}
