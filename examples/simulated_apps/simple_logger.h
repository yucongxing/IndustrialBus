#pragma once

#include <memory>
#include <string>

namespace simulated_apps {

enum class LogLevel {
    Info,
    Warning,
    Error,
};

class Logger {
public:
    virtual ~Logger() = default;
    virtual void log(LogLevel level, const std::string& message) noexcept = 0;

    void info(const std::string& message) noexcept { log(LogLevel::Info, message); }
    void warning(const std::string& message) noexcept {
        log(LogLevel::Warning, message);
    }
    void error(const std::string& message) noexcept {
        log(LogLevel::Error, message);
    }
};

std::shared_ptr<Logger> makeConsoleLogger(std::string component);

}  // namespace simulated_apps
