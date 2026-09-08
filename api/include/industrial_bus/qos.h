#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace industrial_bus {

enum class QosPriority : std::uint8_t {
    Critical = 0,
    High = 1,
    Normal = 2,
    BestEffort = 3,
};

enum class MessageType : std::uint8_t {
    Command,
    Event,
    Request,
    Reply,
    Progress,
    Error,
    Log,
    Heartbeat,
    System,
};

enum class MessageClass : std::uint8_t {
    Reply,
    Accepted,
    FinalState,
    Error,
    Command,
    Request,
    Heartbeat,
    ServiceState,
    Event,
    Progress,
    ErrorLog,
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
        case MessageClass::Heartbeat:
        case MessageClass::ServiceState:
            return QosPriority::High;
        case MessageClass::Event:
        case MessageClass::Progress:
        case MessageClass::ErrorLog:
        case MessageClass::WarnLog:
            return QosPriority::Normal;
        case MessageClass::InfoLog:
        case MessageClass::DebugLog:
            return QosPriority::BestEffort;
    }
    return QosPriority::BestEffort;
}

constexpr MessageType messageTypeFor(MessageClass message_class) noexcept {
    switch (message_class) {
        case MessageClass::Reply:
        case MessageClass::Accepted:
            return MessageType::Reply;
        case MessageClass::FinalState:
        case MessageClass::Event:
            return MessageType::Event;
        case MessageClass::Error: return MessageType::Error;
        case MessageClass::Command: return MessageType::Command;
        case MessageClass::Request: return MessageType::Request;
        case MessageClass::Heartbeat: return MessageType::Heartbeat;
        case MessageClass::ServiceState: return MessageType::System;
        case MessageClass::Progress: return MessageType::Progress;
        case MessageClass::ErrorLog:
        case MessageClass::WarnLog:
        case MessageClass::InfoLog:
        case MessageClass::DebugLog:
            return MessageType::Log;
    }
    return MessageType::Event;
}

constexpr std::string_view messageClassName(MessageClass message_class) noexcept {
    switch (message_class) {
        case MessageClass::Reply: return "Reply";
        case MessageClass::Accepted: return "Accepted";
        case MessageClass::FinalState: return "FinalState";
        case MessageClass::Error: return "Error";
        case MessageClass::Command: return "Command";
        case MessageClass::Request: return "Request";
        case MessageClass::Heartbeat: return "Heartbeat";
        case MessageClass::ServiceState: return "ServiceState";
        case MessageClass::Event: return "Event";
        case MessageClass::Progress: return "Progress";
        case MessageClass::ErrorLog: return "ErrorLog";
        case MessageClass::WarnLog: return "WarnLog";
        case MessageClass::InfoLog: return "InfoLog";
        case MessageClass::DebugLog: return "DebugLog";
    }
    return {};
}

inline std::optional<MessageClass> messageClassFromName(
    std::string_view name) noexcept {
    constexpr MessageClass classes[] = {
        MessageClass::Reply,       MessageClass::Accepted,
        MessageClass::FinalState,  MessageClass::Error,
        MessageClass::Command,     MessageClass::Request,
        MessageClass::Heartbeat,   MessageClass::ServiceState,
        MessageClass::Event,       MessageClass::Progress,
        MessageClass::ErrorLog,    MessageClass::WarnLog,
        MessageClass::InfoLog,     MessageClass::DebugLog,
    };
    for (const auto message_class : classes) {
        if (messageClassName(message_class) == name) {
            return message_class;
        }
    }
    return std::nullopt;
}

constexpr bool isPublishableMessageClass(MessageClass message_class) noexcept {
    switch (message_class) {
        case MessageClass::FinalState:
        case MessageClass::Event:
        case MessageClass::Progress:
        case MessageClass::ErrorLog:
        case MessageClass::WarnLog:
        case MessageClass::InfoLog:
        case MessageClass::DebugLog:
            return true;
        case MessageClass::Reply:
        case MessageClass::Accepted:
        case MessageClass::Error:
        case MessageClass::Command:
        case MessageClass::Request:
        case MessageClass::Heartbeat:
        case MessageClass::ServiceState:
            return false;
    }
    return false;
}

}  // namespace industrial_bus
