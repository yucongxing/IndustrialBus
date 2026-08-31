#pragma once

#include <cstdint>
#include <string>

namespace industrial_bus {

enum class QosPriority : std::uint8_t {
    Critical = 0,
    High = 1,
    Normal = 2,
    BestEffort = 3,
};

enum class MessageClass : std::uint8_t {
    Reply,
    Accepted,
    FinalState,
    Error,
    Command,
    Request,
    ServiceState,
    Event,
    Progress,
    WarnLog,
    InfoLog,
    DebugLog,
};

enum class EnqueueDisposition : std::uint8_t {
    Enqueued,
    Coalesced,
    Rejected,
    CriticalOverflow,
};

struct QueueMetadata {
    QosPriority priority{QosPriority::Normal};
    MessageClass message_class{MessageClass::Event};
    // Required for Progress coalescing; ignored for other message classes.
    std::string coalescing_key;
};

constexpr QosPriority defaultPriority(MessageClass message_class) noexcept {
    switch (message_class) {
        case MessageClass::Reply:
        case MessageClass::Accepted:
        case MessageClass::FinalState:
        case MessageClass::Error:
            return QosPriority::Critical;
        case MessageClass::Command:
        case MessageClass::Request:
        case MessageClass::ServiceState:
            return QosPriority::High;
        case MessageClass::Event:
        case MessageClass::Progress:
        case MessageClass::WarnLog:
            return QosPriority::Normal;
        case MessageClass::InfoLog:
        case MessageClass::DebugLog:
            return QosPriority::BestEffort;
    }
    return QosPriority::BestEffort;
}

}  // namespace industrial_bus
