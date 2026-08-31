#pragma once

#include "industrial_bus/error.h"
#include "industrial_bus/operation_state.h"
#include "industrial_bus/result.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace industrial_bus {

class BusClient;

struct OperationContextSnapshot {
    std::string operation_id;
    std::string correlation_id;
};

OperationContextSnapshot currentOperationContext();

class ScopedOperationContext {
public:
    explicit ScopedOperationContext(OperationContextSnapshot context);
    ~ScopedOperationContext();

    ScopedOperationContext(const ScopedOperationContext&) = delete;
    ScopedOperationContext& operator=(const ScopedOperationContext&) = delete;

private:
    std::size_t depth_{0};
};

struct OperationUpdate {
    std::string operation_id;
    std::string operation_type;
    OperationState state{OperationState::Unspecified};
    std::string stage;
    std::optional<double> stage_progress;
    std::optional<double> overall_progress;
    std::optional<std::uint64_t> current;
    std::optional<std::uint64_t> total;
    std::string message;
    std::chrono::system_clock::time_point start_time{};
    std::chrono::system_clock::time_point end_time{};
    std::optional<Error> error;
};

class OperationGuard {
public:
    OperationGuard(BusClient& bus,
                   std::string operation_id,
                   std::string operation_type,
                   std::string correlation_id = {});
    ~OperationGuard();

    OperationGuard(const OperationGuard&) = delete;
    OperationGuard& operator=(const OperationGuard&) = delete;

    Result<void> reportProgress(std::string stage,
                                std::optional<double> stage_progress = {},
                                std::optional<double> overall_progress = {});
    Result<void> requestCancellation();
    Result<void> complete();
    Result<void> fail(Error error);
    Result<void> cancel();
    OperationState state() const noexcept;

private:
    Result<void> transitionAndReport(OperationState next, std::optional<Error> error = {});

    BusClient* bus_;
    std::string operation_id_;
    std::string operation_type_;
    std::string correlation_id_;
    std::chrono::system_clock::time_point start_time_;
    OperationStateMachine state_machine_;
    std::unique_ptr<ScopedOperationContext> context_;
};

}  // namespace industrial_bus
