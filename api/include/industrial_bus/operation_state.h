#pragma once

#include "industrial_bus/error.h"
#include "industrial_bus/result.h"

#include <cstdint>
#include <mutex>

namespace industrial_bus {

enum class OperationState : std::uint8_t {
    Unspecified = 0,
    Created = 1,
    Running = 2,
    Cancelling = 3,
    Completed = 4,
    Failed = 5,
    Cancelled = 6,
};

enum class OperationObservationState : std::uint8_t {
    Known = 0,
    UnknownDueToPartition = 1,
};

bool isTerminal(OperationState state) noexcept;
bool isValidTransition(OperationState from, OperationState to) noexcept;

class OperationStateMachine {
public:
    explicit OperationStateMachine(OperationState initial = OperationState::Created);

    OperationState state() const;
    Result<void> transitionTo(OperationState next);
    Result<void> requestCancellation();

private:
    mutable std::mutex mutex_;
    OperationState state_;
};

}  // namespace industrial_bus
