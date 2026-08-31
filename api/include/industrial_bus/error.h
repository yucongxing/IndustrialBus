#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace industrial_bus {

enum class ErrorCategory : std::uint8_t {
    Unspecified = 0,
    InvalidArgument = 1,
    FileIo = 2,
    GeometryError = 3,
    OutOfMemory = 4,
    Timeout = 5,
    ConnectionLost = 6,
    ServiceUnavailable = 7,
    Cancelled = 8,
    InternalError = 9,
    ProtocolError = 10,
    PermissionDenied = 11,
    ResourceExhausted = 12,
    HubRestarted = 13,
    OwnerUnavailable = 14,
    IdempotencyConflict = 15,
    ServiceAlreadyRegistered = 16,
};

struct Error {
    ErrorCategory category{ErrorCategory::Unspecified};
    std::int32_t code{0};
    std::string message;
    bool retryable{false};
    std::string operation_id;
    std::map<std::string, std::string> context;

    static Error make(ErrorCategory category,
                      std::string message,
                      std::int32_t code = 0,
                      bool retryable = false) {
        return Error{category, code, std::move(message), retryable, {}, {}};
    }

    static Error invalidArgument(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::InvalidArgument, std::move(message), code);
    }
    static Error fileIo(std::string message, std::int32_t code = 0,
                        bool retryable = false) {
        return make(ErrorCategory::FileIo, std::move(message), code, retryable);
    }
    static Error timeout(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::Timeout, std::move(message), code, true);
    }
    static Error connectionLost(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::ConnectionLost, std::move(message), code, true);
    }
    static Error serviceUnavailable(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::ServiceUnavailable, std::move(message), code, true);
    }
    static Error cancelled(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::Cancelled, std::move(message), code);
    }
    static Error resourceExhausted(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::ResourceExhausted, std::move(message), code, true);
    }
    static Error hubRestarted(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::HubRestarted, std::move(message), code, true);
    }
    static Error ownerUnavailable(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::OwnerUnavailable, std::move(message), code, true);
    }
    static Error permissionDenied(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::PermissionDenied, std::move(message), code);
    }
    static Error idempotencyConflict(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::IdempotencyConflict, std::move(message), code);
    }
    static Error serviceAlreadyRegistered(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::ServiceAlreadyRegistered, std::move(message), code);
    }
    static Error protocol(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::ProtocolError, std::move(message), code);
    }
    static Error internal(std::string message, std::int32_t code = 0) {
        return make(ErrorCategory::InternalError, std::move(message), code);
    }

    explicit operator bool() const noexcept {
        return category != ErrorCategory::Unspecified;
    }
};

inline bool operator==(const Error& lhs, const Error& rhs) {
    return lhs.category == rhs.category && lhs.code == rhs.code &&
           lhs.message == rhs.message && lhs.retryable == rhs.retryable &&
           lhs.operation_id == rhs.operation_id && lhs.context == rhs.context;
}

inline bool operator!=(const Error& lhs, const Error& rhs) {
    return !(lhs == rhs);
}

}  // namespace industrial_bus
