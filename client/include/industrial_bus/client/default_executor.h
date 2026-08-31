#pragma once

#include "industrial_bus/executor.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace industrial_bus::client {

class DefaultExecutor final : public IExecutor {
public:
    explicit DefaultExecutor(std::size_t thread_count = 1);
    ~DefaultExecutor() override;

    DefaultExecutor(const DefaultExecutor&) = delete;
    DefaultExecutor& operator=(const DefaultExecutor&) = delete;

    void post(std::function<void()> task) override;
    void shutdown() noexcept;

private:
    void run();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_{false};
    std::vector<std::thread> workers_;
};

}  // namespace industrial_bus::client
