#pragma once

#include "industrial_bus/error.h"

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace industrial_bus {

template <typename T>
class Result {
    static_assert(!std::is_same<typename std::decay<T>::type, Error>::value,
                  "Result<Error> is ambiguous and is not supported");

public:
    Result(const T& value) : storage_(value) {}
    Result(T&& value) : storage_(std::move(value)) {}
    Result(const Error& error) : storage_(error) {}
    Result(Error&& error) : storage_(std::move(error)) {}

    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Error error) { return Result(std::move(error)); }

    bool hasValue() const noexcept { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const noexcept { return hasValue(); }

    T& value() & {
        ensureValue();
        return std::get<T>(storage_);
    }
    const T& value() const& {
        ensureValue();
        return std::get<T>(storage_);
    }
    T&& value() && {
        ensureValue();
        return std::get<T>(std::move(storage_));
    }

    Error& error() & {
        ensureError();
        return std::get<Error>(storage_);
    }
    const Error& error() const& {
        ensureError();
        return std::get<Error>(storage_);
    }

private:
    void ensureValue() const {
        if (!hasValue()) {
            throw std::logic_error("Result does not contain a value");
        }
    }
    void ensureError() const {
        if (hasValue()) {
            throw std::logic_error("Result does not contain an error");
        }
    }

    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(const Error& error) : error_(error), success_(false) {}
    Result(Error&& error) : error_(std::move(error)), success_(false) {}

    static Result success() { return Result(); }
    static Result failure(Error error) { return Result(std::move(error)); }

    bool hasValue() const noexcept { return success_; }
    explicit operator bool() const noexcept { return success_; }

    void value() const {
        if (!success_) {
            throw std::logic_error("Result does not contain a value");
        }
    }

    Error& error() & {
        ensureError();
        return error_;
    }
    const Error& error() const& {
        ensureError();
        return error_;
    }

private:
    void ensureError() const {
        if (success_) {
            throw std::logic_error("Result does not contain an error");
        }
    }

    Error error_;
    bool success_{true};
};

}  // namespace industrial_bus
