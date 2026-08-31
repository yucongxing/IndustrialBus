#pragma once

#include <future>

namespace industrial_bus {

template <typename T>
using Future = std::future<T>;

}  // namespace industrial_bus
