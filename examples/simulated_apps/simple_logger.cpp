#include "simple_logger.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace simulated_apps {
namespace {

class ConsoleSink final : public LogSink {
public:
    void write(const LogRecord& record) noexcept override {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& output =
                record.level == LogLevel::Error ? std::cerr : std::cout;
            output << record.component << " [" << levelName(record.level)
                   << "]: " << record.message << '\n';
        } catch (...) {
            // A sink must not change application control flow.
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

    std::mutex mutex_;
};

}  // namespace

Logger::Logger(std::string component) : component_(std::move(component)) {}

Logger::SinkId Logger::addSink(std::shared_ptr<LogSink> sink) {
    if (!sink) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = next_sink_id_++;
    sinks_.push_back({id, std::move(sink)});
    return id;
}

void Logger::removeSink(SinkId id) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.erase(
            std::remove_if(sinks_.begin(), sinks_.end(),
                           [id](const SinkEntry& entry) {
                               return entry.id == id;
                           }),
            sinks_.end());
    } catch (...) {
        // Removing a sink must remain safe during shutdown.
    }
}

void Logger::log(LogLevel level, const std::string& message) noexcept {
    try {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        LogRecord record{
            level,
            component_,
            message,
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(),
            static_cast<std::uint64_t>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()))};
        std::vector<std::shared_ptr<LogSink>> sinks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sinks.reserve(sinks_.size());
            for (const auto& entry : sinks_) {
                sinks.push_back(entry.sink);
            }
        }
        for (const auto& sink : sinks) {
            sink->write(record);
        }
    } catch (...) {
        // Logging must not change application control flow.
    }
}

std::shared_ptr<LogSink> makeConsoleSink() {
    return std::make_shared<ConsoleSink>();
}

std::shared_ptr<Logger> makeLogger(
    std::string component,
    std::vector<std::shared_ptr<LogSink>> sinks) {
    auto logger = std::make_shared<Logger>(std::move(component));
    for (auto& sink : sinks) {
        logger->addSink(std::move(sink));
    }
    return logger;
}

std::shared_ptr<Logger> makeConsoleLogger(std::string component) {
    return makeLogger(std::move(component), {makeConsoleSink()});
}

}  // namespace simulated_apps
