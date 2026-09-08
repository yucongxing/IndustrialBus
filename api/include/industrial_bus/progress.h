#pragma once

#include "industrial_bus/operation.h"
#include "industrial_bus/result.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace industrial_bus {

class BusClient;

struct ProgressThrottleOptions {
    std::chrono::milliseconds minimum_interval{100};
    std::chrono::milliseconds maximum_silence{500};
    double immediate_change{0.01};
};

class ProgressThrottle {
public:
    using Sink = std::function<Result<void>(OperationUpdate)>;

    ProgressThrottle(std::string operation_id,
                     std::string operation_type,
                     Sink sink,
                     ProgressThrottleOptions options = {});
    ProgressThrottle(BusClient& bus,
                     std::string operation_id,
                     std::string operation_type,
                     std::string correlation_id = {},
                     ProgressThrottleOptions options = {});
    ~ProgressThrottle();

    ProgressThrottle(const ProgressThrottle&) = delete;
    ProgressThrottle& operator=(const ProgressThrottle&) = delete;

    Result<void> submit(std::string stage,
                        std::optional<double> stage_progress = {},
                        std::optional<double> overall_progress = {},
                        std::string message = {});
    void shutdown() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace industrial_bus
