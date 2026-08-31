#include "industrial_bus/operation.h"

#include "industrial_bus/bus_client.h"
#include "industrial_bus/ids.h"

#include <utility>
#include <vector>

namespace industrial_bus {
namespace {
thread_local std::vector<OperationContextSnapshot> g_operation_contexts;
}

OperationContextSnapshot currentOperationContext() {
    if (g_operation_contexts.empty()) {
        return {};
    }
    return g_operation_contexts.back();
}

ScopedOperationContext::ScopedOperationContext(OperationContextSnapshot context)
    : depth_(g_operation_contexts.size()) {
    g_operation_contexts.push_back(std::move(context));
}

ScopedOperationContext::~ScopedOperationContext() {
    if (g_operation_contexts.size() > depth_) {
        g_operation_contexts.resize(depth_);
    }
}

OperationGuard::OperationGuard(BusClient& bus,
                               std::string operation_id,
                               std::string operation_type,
                               std::string correlation_id)
    : bus_(&bus),
      operation_id_(std::move(operation_id)),
      operation_type_(std::move(operation_type)),
      correlation_id_(correlation_id.empty() ? generateUuidV4()
                                             : std::move(correlation_id)),
      start_time_(std::chrono::system_clock::now()),
      state_machine_(OperationState::Created),
      context_(std::make_unique<ScopedOperationContext>(
          OperationContextSnapshot{operation_id_, correlation_id_})) {
    OperationUpdate created;
    created.operation_id = operation_id_;
    created.operation_type = operation_type_;
    created.state = OperationState::Created;
    created.start_time = start_time_;
    bus_->reportOperation(std::move(created), correlation_id_);
    transitionAndReport(OperationState::Running);
}

OperationGuard::~OperationGuard() {
    if (!isTerminal(state_machine_.state())) {
        try {
            auto error = Error::internal("operation guard destroyed without terminal state");
            error.operation_id = operation_id_;
            transitionAndReport(OperationState::Failed, std::move(error));
        } catch (...) {
        }
    }
}

Result<void> OperationGuard::reportProgress(std::string stage,
                                            std::optional<double> stage_progress,
                                            std::optional<double> overall_progress) {
    if (isTerminal(state_machine_.state())) {
        return Error::invalidArgument("cannot report progress for a terminal operation");
    }
    const auto valid = [](const std::optional<double>& value) {
        return !value || (*value >= 0.0 && *value <= 1.0);
    };
    if (!valid(stage_progress) || !valid(overall_progress)) {
        return Error::invalidArgument("progress must be between zero and one");
    }
    OperationUpdate update;
    update.operation_id = operation_id_;
    update.operation_type = operation_type_;
    update.state = state_machine_.state();
    update.stage = std::move(stage);
    update.stage_progress = stage_progress;
    update.overall_progress = overall_progress;
    update.start_time = start_time_;
    return bus_->reportOperation(std::move(update), correlation_id_);
}

Result<void> OperationGuard::requestCancellation() {
    return transitionAndReport(OperationState::Cancelling);
}

Result<void> OperationGuard::complete() {
    return transitionAndReport(OperationState::Completed);
}

Result<void> OperationGuard::fail(Error error) {
    error.operation_id = operation_id_;
    return transitionAndReport(OperationState::Failed, std::move(error));
}

Result<void> OperationGuard::cancel() {
    return transitionAndReport(OperationState::Cancelled);
}

OperationState OperationGuard::state() const noexcept {
    return state_machine_.state();
}

Result<void> OperationGuard::transitionAndReport(OperationState next,
                                                 std::optional<Error> error) {
    auto transitioned = state_machine_.transitionTo(next);
    if (!transitioned) {
        return transitioned;
    }
    OperationUpdate update;
    update.operation_id = operation_id_;
    update.operation_type = operation_type_;
    update.state = next;
    update.start_time = start_time_;
    if (isTerminal(next)) {
        update.end_time = std::chrono::system_clock::now();
    }
    update.error = std::move(error);
    return bus_->reportOperation(std::move(update), correlation_id_);
}

}  // namespace industrial_bus
