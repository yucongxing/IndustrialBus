#include "industrial_bus/client/default_executor.h"

#include <algorithm>
#include <utility>

namespace industrial_bus::client {

DefaultExecutor::DefaultExecutor(std::size_t thread_count) {
    thread_count = std::max<std::size_t>(1, thread_count);
    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this] { run(); });
    }
}

DefaultExecutor::~DefaultExecutor() {
    shutdown();
}

void DefaultExecutor::post(std::function<void()> task) {
    if (!task) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void DefaultExecutor::shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void DefaultExecutor::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (...) {
            // A user callback is isolated from the executor and other callbacks.
        }
    }
}

}  // namespace industrial_bus::client
