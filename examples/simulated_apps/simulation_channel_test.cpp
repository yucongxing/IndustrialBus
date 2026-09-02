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
}

}  // namespace
