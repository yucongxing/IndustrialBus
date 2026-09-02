#pragma once

#include "industrial_bus/topic.h"

#include "cmp.pb.h"
#include "logging.pb.h"
#include "system.pb.h"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace industrial_bus::examples {

inline bool isValidChannelId(std::string_view value) noexcept {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

// Owns all strings referenced by its typed Topics. Keep this object alive for
// every subscription/handler which was created from it.
class ChannelTopics {
public:
    explicit ChannelTopics(std::string channel_id)
        : channel_id_(std::move(channel_id)),
          prefix_("cmp.channel." + channel_id_),
          run_name_(prefix_ + ".simulation.run"),
          progress_name_(prefix_ + ".simulation.progress"),
          completed_name_(prefix_ + ".simulation.completed"),
          failed_name_(prefix_ + ".simulation.failed"),
          log_name_("application.log." + channel_id_) {}

    ChannelTopics(const ChannelTopics&) = delete;
    ChannelTopics& operator=(const ChannelTopics&) = delete;
    ChannelTopics(ChannelTopics&&) = delete;
    ChannelTopics& operator=(ChannelTopics&&) = delete;

    const std::string& channelId() const noexcept { return channel_id_; }

    CommandTopic<cmp::v1::RunSimulation, system::v1::CommandAck>
    runSimulation() const noexcept {
        return {run_name_, "industrial_bus.cmp.v1.RunSimulation",
                "industrial_bus.system.v1.CommandAck"};
    }

    EventTopic<cmp::v1::SimulationProgress> simulationProgress() const noexcept {
        return {progress_name_, "industrial_bus.cmp.v1.SimulationProgress"};
    }

    EventTopic<cmp::v1::SimulationCompleted>
    simulationCompleted() const noexcept {
        return {completed_name_, "industrial_bus.cmp.v1.SimulationCompleted"};
    }

    EventTopic<cmp::v1::SimulationFailed> simulationFailed() const noexcept {
        return {failed_name_, "industrial_bus.cmp.v1.SimulationFailed"};
    }

    EventTopic<logging::v1::LogEvent> applicationLog() const noexcept {
        return {log_name_, "industrial_bus.logging.v1.LogEvent"};
    }

private:
    std::string channel_id_;
    std::string prefix_;
    std::string run_name_;
    std::string progress_name_;
    std::string completed_name_;
    std::string failed_name_;
    std::string log_name_;
};

}  // namespace industrial_bus::examples
