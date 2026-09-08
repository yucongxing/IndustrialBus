#pragma once

#include "industrial_bus/runtime/protocol_validation.h"
#include "industrial_bus/topic_catalog.h"

#include <iterator>
#include <vector>

namespace industrial_bus::runtime::test {

inline TopicCatalogView topicCatalog() {
    static const std::vector<TopicBinding> bindings = [] {
        const auto production = productionTopicCatalog();
        std::vector<TopicBinding> result(production.begin(), production.end());
        const TopicBinding extra[] = {
            {"kTestCodecCommand", "test.codec", MessageType::Command, CatalogPayloadRole::CommandInput, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecEvent", "test.codec", MessageType::Event, CatalogPayloadRole::EventFact, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecRequest", "test.codec", MessageType::Request, CatalogPayloadRole::RequestInput, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecReply", "test.codec", MessageType::Reply, CatalogPayloadRole::ResponseOutput, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecProgress", "test.codec", MessageType::Progress, CatalogPayloadRole::EventFact, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecError", "test.codec", MessageType::Error, CatalogPayloadRole::ErrorDetail, "industrial_bus.system.v1.Error", 1, 65536, ProtocolPayloadValidatorKind::None},
            {"kTestCodecLog", "test.codec", MessageType::Log, CatalogPayloadRole::EventFact, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecHeartbeat", "test.codec", MessageType::Heartbeat, CatalogPayloadRole::SystemPayload, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecSystem", "test.codec", MessageType::System, CatalogPayloadRole::SystemPayload, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecMetadataEvent", "test.codec.metadata", MessageType::Event, CatalogPayloadRole::EventFact, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecMetadataCommand", "test.codec.metadata", MessageType::Command, CatalogPayloadRole::CommandInput, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecMetadataRequest", "test.codec.metadata", MessageType::Request, CatalogPayloadRole::RequestInput, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCodecMetadataReply", "test.codec.metadata", MessageType::Reply, CatalogPayloadRole::ResponseOutput, "test.Bytes", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestRequestError", "test.request", MessageType::Reply, CatalogPayloadRole::ErrorDetail, "industrial_bus.system.v1.Error", 1, 65536, ProtocolPayloadValidatorKind::None},
            {"kTestEvent", "test.event", MessageType::Event, CatalogPayloadRole::EventFact, "test.StringEvent", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCommand", "test.command", MessageType::Command, CatalogPayloadRole::CommandInput, "test.StringCommand", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestCommandAck", "test.command", MessageType::Reply, CatalogPayloadRole::CommandAck, "industrial_bus.system.v1.CommandAck", 1, 65536, ProtocolPayloadValidatorKind::None},
            {"kTestCommandError", "test.command", MessageType::Reply, CatalogPayloadRole::ErrorDetail, "industrial_bus.system.v1.Error", 1, 65536, ProtocolPayloadValidatorKind::None},
            {"kTestMissing", "test.missing", MessageType::Command, CatalogPayloadRole::CommandInput, "test.MissingCommand", 1, 4096, ProtocolPayloadValidatorKind::None},
            {"kTestMissingError", "test.missing", MessageType::Reply, CatalogPayloadRole::ErrorDetail, "industrial_bus.system.v1.Error", 1, 65536, ProtocolPayloadValidatorKind::None},
            {"kTestIsolatedBroadcast", "test.isolated-broadcast", MessageType::Event, CatalogPayloadRole::EventFact, "test.StringEvent", 1, 4096, ProtocolPayloadValidatorKind::None},
        };
        result.insert(result.end(), std::begin(extra), std::end(extra));
        return result;
    }();
    return {bindings.data(), bindings.size()};
}

inline ProtocolValidator protocolValidator() {
    return ProtocolValidator(topicCatalog());
}

}  // namespace industrial_bus::runtime::test
