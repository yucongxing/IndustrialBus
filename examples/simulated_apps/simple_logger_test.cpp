#include "simple_logger.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

class CapturingSink final : public simulated_apps::LogSink {
public:
    void write(const simulated_apps::LogRecord& record) noexcept override {
        records.push_back(record);
    }

    std::vector<simulated_apps::LogRecord> records;
};

TEST(SimpleLoggerTest, FormatsLevelsAndComponent) {
    auto logger = simulated_apps::makeConsoleLogger("Demo");

    testing::internal::CaptureStdout();
    logger->info("connected");
    logger->warning("retrying");
    const auto standard_output = testing::internal::GetCapturedStdout();
    EXPECT_NE(standard_output.find("Demo [info]: connected"),
              std::string::npos);
    EXPECT_NE(standard_output.find("Demo [warning]: retrying"),
              std::string::npos);

    testing::internal::CaptureStderr();
    logger->error("failed");
    const auto error_output = testing::internal::GetCapturedStderr();
    EXPECT_NE(error_output.find("Demo [error]: failed"), std::string::npos);
}

TEST(SimpleLoggerTest, FansOutAndRemovesSinks) {
    auto first = std::make_shared<CapturingSink>();
    auto second = std::make_shared<CapturingSink>();
    auto logger = simulated_apps::makeLogger("Demo", {first});
    const auto second_id = logger->addSink(second);

    logger->info("one");
    logger->removeSink(second_id);
    logger->error("two");

    ASSERT_EQ(first->records.size(), 2U);
    EXPECT_EQ(first->records[0].component, "Demo");
    EXPECT_EQ(first->records[0].message, "one");
    EXPECT_GT(first->records[0].timestamp_unix_ns, 0);
    EXPECT_NE(first->records[0].thread_id, 0U);
    ASSERT_EQ(second->records.size(), 1U);
    EXPECT_EQ(second->records[0].message, "one");
}

}  // namespace
