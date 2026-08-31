#include "industrial_bus/operation_state.h"

#include <string>

namespace industrial_bus {
namespace {

const char* stateName(OperationState state) noexcept {
    switch (state) {
        case OperationState::Unspecified: return "UNSPECIFIED";
        case OperationState::Created: return "CREATED";
        case OperationState::Running: return "RUNNING";
        case OperationState::Cancelling: return "CANCELLING";
        case OperationState::Completed: return "COMPLETED";
        case OperationState::Failed: return "FAILED";
        case OperationState::Cancelled: return "CANCELLED";
    }
    return "UNKNOWN";
}

}  // namespace

bool isTerminal(OperationState state) noexcept {
    return state == OperationState::Completed || state == OperationState::Failed ||
           state == OperationState::Cancelled;
}

bool isValidTransition(OperationState from, OperationState to) noexcept {
    switch (from) {
        case OperationState::Created:
            return to == OperationState::Running || to == OperationState::Cancelled;
        case OperationState::Running:
            return to == OperationState::Completed || to == OperationState::Failed ||
                   to == OperationState::Cancelling;
        case OperationState::Cancelling:
            return to == OperationState::Cancelled || to == OperationState::Completed ||
                   to == OperationState::Failed;
        case OperationState::Unspecified:
        case OperationState::Completed:
        case OperationState::Failed:
        case OperationState::Cancelled:
            return false;
    }
    return false;
}

OperationStateMachine::OperationStateMachine(OperationState initial) : state_(initial) {}

OperationState OperationStateMachine::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

Result<void> OperationStateMachine::transitionTo(OperationState next) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isValidTransition(state_, next)) {
        return Error::invalidArgument(
            std::string("invalid operation transition: ") + stateName(state_) + " -> " +
            stateName(next));
    }
    state_ = next;
    return {};
}

Result<void> OperationStateMachine::requestCancellation() {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (state_) {
        case OperationState::Created:
            state_ = OperationState::Cancelled;
            return {};
        case OperationState::Running:
            state_ = OperationState::Cancelling;
            return {};
        case OperationState::Cancelling:
        case OperationState::Completed:
        case OperationState::Failed:
        case OperationState::Cancelled:
            // Cancellation is best effort and idempotent. Terminal state remains authoritative.
            return {};
        case OperationState::Unspecified:
            return Error::invalidArgument("cannot cancel an unspecified operation");
    }
    return Error::internal("unrecognized operation state");
}

}  // namespace industrial_bus
