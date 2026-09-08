#pragma once

#include "industrial_bus/topics.generated.h"

#include "logging.pb.h"

#include <string>
#include <string_view>

namespace industrial_bus::examples {

inline MessageClass logMessageClass(logging::v1::LogLevel level) noexcept {
    switch (level) {
        case logging::v1::WARN: return MessageClass::WarnLog;
        case logging::v1::LOG_ERROR: return MessageClass::ErrorLog;
        case logging::v1::INFO: return MessageClass::InfoLog;
        case logging::v1::DEBUG: return MessageClass::DebugLog;
        case logging::v1::LOG_LEVEL_UNSPECIFIED: return MessageClass::InfoLog;
        default: return MessageClass::InfoLog;
    }
}

inline bool isValidChannelId(std::string_view value) noexcept {
    return isValidUuid(value);
}

// Owns all strings referenced by its typed Topics. Keep this object alive for
// every subscription/handler which was created from it.
using ChannelTopics = topics::ChannelTopics;

}  // namespace industrial_bus::examples
