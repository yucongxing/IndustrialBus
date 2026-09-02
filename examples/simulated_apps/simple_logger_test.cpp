#include "simple_logger.h"

#include <gtest/gtest.h>

#include <string>

namespace {

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

}  // namespace
