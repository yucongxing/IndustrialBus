#pragma once

#include <functional>
#include <utility>

namespace industrial_bus {

class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept
        : cancel_(std::move(other.cancel_)) {}

    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            cancel();
            cancel_ = std::move(other.cancel_);
        }
        return *this;
    }

    ~Subscription() { cancel(); }

    void cancel() noexcept {
        if (!cancel_) {
            return;
        }
        auto fn = std::move(cancel_);
        try {
            fn();
        } catch (...) {
            // Destruction and explicit cancellation are noexcept by design.
        }
    }

    explicit operator bool() const noexcept { return static_cast<bool>(cancel_); }

private:
    std::function<void()> cancel_;
};

}  // namespace industrial_bus
