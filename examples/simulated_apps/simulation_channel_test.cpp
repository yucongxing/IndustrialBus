#include "simulation_app.h"
#include "simulation_topics.h"

#include <gtest/gtest.h>

#include <string>

namespace {

TEST(SimulationChannelTest, GeneratesAndValidatesChannelIdentifiers) {
    const auto generated = simulated_apps::generateChannelId();
    EXPECT_TRUE(simulated_apps::isValidChannelId(generated));
    EXPECT_FALSE(simulated_apps::isValidChannelId(""));
    EXPECT_FALSE(simulated_apps::isValidChannelId("channel-a"));
    EXPECT_FALSE(simulated_apps::isValidChannelId(
        "12345678-1234-1234-1234-12345678901/"));
}

TEST(SimulationChannelTest, BuildsOwnedChannelSpecificTopics) {
    const std::string channel = "12345678-1234-1234-1234-123456789012";
    industrial_bus::examples::ChannelTopics topics(channel);

    EXPECT_EQ(topics.channelId(), channel);
    EXPECT_EQ(topics.runSimulation().name,
              "cmp.channel." + channel + ".simulation.run");
    EXPECT_EQ(topics.simulationProgress().name,
              "cmp.channel." + channel + ".simulation.progress");
    EXPECT_EQ(topics.simulationCompleted().name,
              "cmp.channel." + channel + ".simulation.completed");
    EXPECT_EQ(topics.simulationFailed().name,
              "cmp.channel." + channel + ".simulation.failed");
    EXPECT_EQ(topics.applicationLog().name, "application.log." + channel);
    EXPECT_EQ(topics.simulationProgress().message_class,
              industrial_bus::MessageClass::Progress);
    EXPECT_EQ(topics.simulationCompleted().message_class,
              industrial_bus::MessageClass::FinalState);
    EXPECT_EQ(topics.simulationFailed().message_class,
              industrial_bus::MessageClass::FinalState);
    EXPECT_EQ(topics.applicationLog().message_class,
              industrial_bus::MessageClass::InfoLog);
}

TEST(SimulationChannelTest, MapsEveryRuntimeLogSeverityToSemanticQos) {
    using industrial_bus::MessageClass;
    using industrial_bus::examples::logMessageClass;
    EXPECT_EQ(logMessageClass(industrial_bus::logging::v1::LOG_ERROR),
              MessageClass::ErrorLog);
    EXPECT_EQ(logMessageClass(industrial_bus::logging::v1::WARN),
              MessageClass::WarnLog);
    EXPECT_EQ(logMessageClass(industrial_bus::logging::v1::INFO),
              MessageClass::InfoLog);
    EXPECT_EQ(logMessageClass(industrial_bus::logging::v1::DEBUG),
              MessageClass::DebugLog);
}

}  // namespace
