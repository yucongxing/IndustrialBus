#include "industrial_bus/progress.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace industrial_bus {

class ProgressThrottle::Impl {
public:
    Impl(std::string operation_id_value,
         std::string operation_type_value,
         Sink sink_value,
         ProgressThrottleOptions options_value)
        : operation_id(std::move(operation_id_value)),
          operation_type(std::move(operation_type_value)),
          sink(std::move(sink_value)),
          options(options_value),
          worker([this] { run(); }) {}

    ~Impl() { stop(); }

    Result<void> enqueue(std::string stage,
                         std::optional<double> stage_progress,
                         std::optional<double> overall_progress,
                         std::string message) {
        const auto valid = [](const std::optional<double>& value) {
            return !value || (*value >= 0.0 && *value <= 1.0);
        };
        if (operation_id.empty() || operation_type.empty() || !sink) {
            return Error::invalidArgument("progress throttle is not configured");
        }
        if (!valid(stage_progress) || !valid(overall_progress)) {
            return Error::invalidArgument("progress must be between zero and one");
        }

        OperationUpdate update;
        update.operation_id = operation_id;
        update.operation_type = operation_type;
        update.state = OperationState::Running;
        update.stage = std::move(stage);
        update.stage_progress = stage_progress;
        update.overall_progress = overall_progress;
        update.message = std::move(message);

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) {
                return Error::make(ErrorCategory::Cancelled,
                                   "progress throttle is stopped");
            }
            latest = std::move(update);  // Coalesce by operation: retain only latest.
            has_latest = true;
        }
        cv.notify_one();
        return {};
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) {
                return;
            }
            stopping = true;
        }
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

private:
    using Clock = std::chrono::steady_clock;

    bool significant(const OperationUpdate& update) const {
        if (!sent_once || update.stage != last_sent.stage) {
            return true;
        }
        const auto changed = [this](const std::optional<double>& current,
                                    const std::optional<double>& prior) {
            if (current.has_value() != prior.has_value()) {
                return true;
            }
            return current && std::fabs(*current - *prior) >= options.immediate_change;
        };
        return changed(update.stage_progress, last_sent.stage_progress) ||
               changed(update.overall_progress, last_sent.overall_progress);
    }

    void emit(OperationUpdate update) noexcept {
        try {
            sink(update);
        } catch (...) {
        }
        last_sent = std::move(update);
        last_sent_at = Clock::now();
        sent_once = true;
    }

    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            cv.wait(lock, [this] { return stopping || has_latest; });
            if (stopping) {
                if (has_latest) {
                    auto update = std::move(latest);
                    has_latest = false;
                    lock.unlock();
                    emit(std::move(update));
                }
                return;
            }

            const auto now = Clock::now();
            const bool emit_now = !sent_once || significant(latest) ||
                                  now - last_sent_at >= options.maximum_silence;
            if (emit_now) {
                auto update = std::move(latest);
                has_latest = false;
                lock.unlock();
                emit(std::move(update));
                lock.lock();
                continue;
            }

            const auto next = std::min(last_sent_at + options.minimum_interval,
                                       last_sent_at + options.maximum_silence);
            cv.wait_until(lock, next, [this] { return stopping; });
            if (!stopping && has_latest && Clock::now() >= next) {
                auto update = std::move(latest);
                has_latest = false;
                lock.unlock();
                emit(std::move(update));
                lock.lock();
            }
        }
    }

    std::string operation_id;
    std::string operation_type;
    Sink sink;
    ProgressThrottleOptions options;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping{false};
    bool has_latest{false};
    OperationUpdate latest;
    OperationUpdate last_sent;
    bool sent_once{false};
    Clock::time_point last_sent_at{};
    std::thread worker;
};

ProgressThrottle::ProgressThrottle(std::string operation_id,
                                   std::string operation_type,
                                   Sink sink,
                                   ProgressThrottleOptions options)
    : impl_(std::make_unique<Impl>(std::move(operation_id),
                                   std::move(operation_type), std::move(sink),
                                   options)) {}

ProgressThrottle::~ProgressThrottle() = default;

Result<void> ProgressThrottle::submit(std::string stage,
                                      std::optional<double> stage_progress,
                                      std::optional<double> overall_progress,
                                      std::string message) {
    return impl_->enqueue(std::move(stage), stage_progress, overall_progress,
                          std::move(message));
}

void ProgressThrottle::shutdown() noexcept {
    impl_->stop();
}

}  // namespace industrial_bus
