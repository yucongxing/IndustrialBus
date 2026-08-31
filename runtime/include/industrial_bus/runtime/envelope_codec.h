#pragma once

#include "industrial_bus/client/runtime_adapter.h"
#include "industrial_bus/commhub/types.h"
#include "industrial_bus/result.h"
#include "industrial_bus/transport/transport.h"

#include <string>

namespace industrial_bus::runtime {

struct SourceIdentity {
    std::string node_id;
    std::string service_name;
    std::string instance_id;
};

Result<transport::ByteBuffer> encodeOutbound(
    const client::OutboundMessage& message,
    const SourceIdentity& source);

Result<client::InboundMessage> decodeInbound(
    const transport::ByteBuffer& frame);

Result<commhub::RouteMessage> decodeRouteMessage(
    const transport::ByteBuffer& frame);

}  // namespace industrial_bus::runtime
