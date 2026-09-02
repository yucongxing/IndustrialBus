#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace simulated_apps {

enum class LogLevel {
    Info,
    Warning,
    Error,
};

struct LogRecord {
    LogLevel level{LogLevel::Info};
    std::string component;
    std::string message;
    std::int64_t timestamp_unix_ns{0};
    std::uint64_t thread_id{0};
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) noexcept = 0;
};

class Logger final {
public:
    using SinkId = std::size_t;

    explicit Logger(std::string component);

    SinkId addSink(std::shared_ptr<LogSink> sink);
    void removeSink(SinkId id) noexcept;
    void log(LogLevel level, const std::string& message) noexcept;

    void info(const std::string& message) noexcept { log(LogLevel::Info, message); }
    void warning(const std::string& message) noexcept {
        log(LogLevel::Warning, message);
    }
    void error(const std::string& message) noexcept {
        log(LogLevel::Error, message);
    }

private:
    struct SinkEntry {
        SinkId id;
        std::shared_ptr<LogSink> sink;
    };

    std::string component_;
    std::mutex mutex_;
    std::vector<SinkEntry> sinks_;
    SinkId next_sink_id_{1};
};

std::shared_ptr<LogSink> makeConsoleSink();
std::shared_ptr<Logger> makeLogger(
    std::string component,
    std::vector<std::shared_ptr<LogSink>> sinks = {});
std::shared_ptr<Logger> makeConsoleLogger(std::string component);

}  // namespace simulated_apps
