#pragma once

#include "industrial_bus/qos.h"

#include <string_view>

namespace industrial_bus {

template <typename Event>
struct EventTopic {
    std::string_view name;
    std::string_view payload_type;
    MessageClass message_class{MessageClass::Event};
};

template <typename Command, typename Ack>
struct CommandTopic {
    std::string_view name;
    std::string_view command_type;
    std::string_view ack_type;
};

template <typename Request, typename Response>
struct RequestTopic {
    std::string_view name;
    std::string_view request_type;
    std::string_view response_type;
};

template <typename Payload>
struct SystemTopic {
    std::string_view name;
    std::string_view payload_type;
    MessageType message_type{MessageType::System};
};

}  // namespace industrial_bus
