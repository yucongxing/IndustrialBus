#pragma once

#include <functional>

namespace industrial_bus {

class IExecutor {
public:
    virtual ~IExecutor() = default;
    virtual void post(std::function<void()> task) = 0;
};

}  // namespace industrial_bus
