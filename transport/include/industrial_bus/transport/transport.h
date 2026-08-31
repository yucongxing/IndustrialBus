#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace industrial_bus::transport {

using ByteBuffer = std::vector<std::uint8_t>;

class ByteView {
public:
    ByteView() = default;
    ByteView(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    explicit ByteView(const ByteBuffer& bytes) : ByteView(bytes.data(), bytes.size()) {}

    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
};

enum class ErrorCode {
    none = 0,
    invalid_argument,
    not_connected,
    already_listening,
    endpoint_unavailable,
    resource_exhausted,
    transport_failure,
    closed,
};

struct Error {
    ErrorCode code{ErrorCode::none};
    std::string message;
};

template <typename T>
class Result {
public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Error error) { return Result(std::move(error)); }

    explicit operator bool() const noexcept { return value_.has_value(); }
    bool hasValue() const noexcept { return value_.has_value(); }
    T& value() & { return value_.value(); }
    const T& value() const& { return value_.value(); }
    T&& value() && { return std::move(value_.value()); }
    const Error& error() const noexcept { return error_; }

private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<T> value_;
    Error error_;
};

template <>
class Result<void> {
public:
    static Result success() { return Result(true, {}); }
    static Result failure(Error error) { return Result(false, std::move(error)); }

    explicit operator bool() const noexcept { return success_; }
    bool hasValue() const noexcept { return success_; }
    const Error& error() const noexcept { return error_; }

private:
    Result(bool success, Error error) : success_(success), error_(std::move(error)) {}

    bool success_{false};
    Error error_;
};

template <typename T>
using Future = std::future<T>;

struct Endpoint {
    std::string value;

    bool empty() const noexcept { return value.empty(); }
};

enum class ConnectionState {
    disconnected,
    connecting,
    connected,
    closing,
    closed,
    failed,
};

using ReceiveHandler = std::function<void(ByteBuffer)>;

class IConnection {
public:
    virtual ~IConnection() = default;
    virtual Result<void> send(ByteView frame) = 0;
    virtual void setReceiveHandler(ReceiveHandler handler) = 0;
    virtual ConnectionState state() const = 0;
    virtual void close() = 0;
};

using AcceptHandler = std::function<void(std::shared_ptr<IConnection>)>;

class IConnector {
public:
    virtual ~IConnector() = default;
    virtual Future<Result<std::shared_ptr<IConnection>>> connect(const Endpoint& endpoint) = 0;
};

class IListener {
public:
    virtual ~IListener() = default;
    virtual Result<void> listen(const Endpoint& endpoint, AcceptHandler handler) = 0;
    virtual void close() = 0;
};

}  // namespace industrial_bus::transport
