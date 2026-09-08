// Generated from protocol/catalog/topics.textproto. Do not edit by hand.
#pragma once

#include "industrial_bus/ids.h"
#include "industrial_bus/topic.h"

#include "cmp.pb.h"
#include "logging.pb.h"
#include "operation.pb.h"
#include "system.pb.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace industrial_bus::topics {

inline constexpr std::string_view kCatalogFingerprint = "fnv1a64:ef6b516e84bfdb4c";

inline constexpr SystemTopic<system::v1::ServiceRegistration> kServiceRegisterInput{
    "system.service.register", "industrial_bus.system.v1.ServiceRegistration", MessageType::System};
inline constexpr SystemTopic<system::v1::ServiceRegistrationAck> kServiceRegisterAck{
    "system.service.register", "industrial_bus.system.v1.ServiceRegistrationAck", MessageType::Reply};
inline constexpr SystemTopic<system::v1::Error> kServiceRegisterError{
    "system.service.register", "industrial_bus.system.v1.Error", MessageType::Reply};
inline constexpr SystemTopic<system::v1::SubscriptionUpdate> kSubscriptionUpdateInput{
    "system.subscription.update", "industrial_bus.system.v1.SubscriptionUpdate", MessageType::System};
inline constexpr SystemTopic<system::v1::SubscriptionAck> kSubscriptionUpdateAck{
    "system.subscription.update", "industrial_bus.system.v1.SubscriptionAck", MessageType::Reply};
inline constexpr SystemTopic<system::v1::Error> kSubscriptionUpdateError{
    "system.subscription.update", "industrial_bus.system.v1.Error", MessageType::Reply};
inline constexpr SystemTopic<system::v1::ServiceDisconnected> kServiceDisconnect{
    "system.service.disconnect", "industrial_bus.system.v1.ServiceDisconnected", MessageType::System};
inline constexpr SystemTopic<system::v1::ServiceDirectoryUpdate> kServiceDirectory{
    "system.service.directory", "industrial_bus.system.v1.ServiceDirectoryUpdate", MessageType::System};
inline constexpr SystemTopic<system::v1::Heartbeat> kHeartbeatInput{
    "system.heartbeat", "industrial_bus.system.v1.Heartbeat", MessageType::Heartbeat};
inline constexpr SystemTopic<system::v1::HeartbeatAck> kHeartbeatAck{
    "system.heartbeat", "industrial_bus.system.v1.HeartbeatAck", MessageType::Reply};
inline constexpr SystemTopic<system::v1::Error> kHeartbeatError{
    "system.heartbeat", "industrial_bus.system.v1.Error", MessageType::Reply};
inline constexpr EventTopic<operation::v1::OperationEvent> kOperationEvent{
    "system.operation.event", "industrial_bus.operation.v1.OperationEvent", MessageClass::Event};
inline constexpr EventTopic<operation::v1::ProgressEvent> kOperationProgress{
    "system.operation.progress", "industrial_bus.operation.v1.ProgressEvent", MessageClass::Progress};
inline constexpr std::string_view kRunSimulationInputPattern = "cmp.channel.{channel_uuid}.simulation.run";
inline constexpr std::string_view kRunSimulationAckPattern = "cmp.channel.{channel_uuid}.simulation.run";
inline constexpr std::string_view kRunSimulationErrorPattern = "cmp.channel.{channel_uuid}.simulation.run";
inline constexpr std::string_view kSimulationProgressPattern = "cmp.channel.{channel_uuid}.simulation.progress";
inline constexpr std::string_view kSimulationCompletedPattern = "cmp.channel.{channel_uuid}.simulation.completed";
inline constexpr std::string_view kSimulationFailedPattern = "cmp.channel.{channel_uuid}.simulation.failed";
inline constexpr std::string_view kApplicationLogPattern = "application.log.{channel_uuid}";

class ChannelTopics {
public:
    explicit ChannelTopics(std::string channel_uuid)
        : channel_uuid_(std::move(channel_uuid)),
          prefix_("cmp.channel." + channel_uuid_),
          run_(prefix_ + ".simulation.run"),
          progress_(prefix_ + ".simulation.progress"),
          completed_(prefix_ + ".simulation.completed"),
          failed_(prefix_ + ".simulation.failed"),
          log_("application.log." + channel_uuid_) {
        if (!isValidUuid(channel_uuid_)) {
            throw std::invalid_argument("channel_uuid must be an RFC 4122 UUID segment");
        }
    }

    ChannelTopics(const ChannelTopics&) = delete;
    ChannelTopics& operator=(const ChannelTopics&) = delete;
    ChannelTopics(ChannelTopics&&) = delete;
    ChannelTopics& operator=(ChannelTopics&&) = delete;

    const std::string& channelId() const noexcept { return channel_uuid_; }

    CommandTopic<cmp::v1::RunSimulation, system::v1::CommandAck>
    runSimulation() const noexcept {
        return {run_, "industrial_bus.cmp.v1.RunSimulation",
                "industrial_bus.system.v1.CommandAck"};
    }

    EventTopic<cmp::v1::SimulationProgress> simulationProgress() const noexcept {
        return {progress_, "industrial_bus.cmp.v1.SimulationProgress",
                MessageClass::Progress};
    }

    EventTopic<cmp::v1::SimulationCompleted> simulationCompleted() const noexcept {
        return {completed_, "industrial_bus.cmp.v1.SimulationCompleted",
                MessageClass::FinalState};
    }

    EventTopic<cmp::v1::SimulationFailed> simulationFailed() const noexcept {
        return {failed_, "industrial_bus.cmp.v1.SimulationFailed",
                MessageClass::FinalState};
    }

    EventTopic<logging::v1::LogEvent> applicationLog() const noexcept {
        return {log_, "industrial_bus.logging.v1.LogEvent",
                MessageClass::InfoLog};
    }

private:
    std::string channel_uuid_;
    std::string prefix_;
    std::string run_;
    std::string progress_;
    std::string completed_;
    std::string failed_;
    std::string log_;
};

}  // namespace industrial_bus::topics
