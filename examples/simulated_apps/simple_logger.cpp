#include "simple_logger.h"

#include <iostream>
#include <mutex>
#include <utility>

namespace simulated_apps {
namespace {

class ConsoleLogger final : public Logger {
public:
    explicit ConsoleLogger(std::string component)
        : component_(std::move(component)) {}

    void log(LogLevel level, const std::string& message) noexcept override {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& output = level == LogLevel::Error ? std::cerr : std::cout;
            output << component_ << " [" << levelName(level) << "]: "
                   << message << '\n';
        } catch (...) {
            // Logging must not change application control flow.
        }
    }

private:
    static const char* levelName(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::Info: return "info";
            case LogLevel::Warning: return "warning";
            case LogLevel::Error: return "error";
        }
        return "unknown";
    }

    std::string component_;
    std::mutex mutex_;
};

}  // namespace

std::shared_ptr<Logger> makeConsoleLogger(std::string component) {
    return std::make_shared<ConsoleLogger>(std::move(component));
}

}  // namespace simulated_apps
