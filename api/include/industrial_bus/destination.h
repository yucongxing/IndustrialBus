#pragma once

#include <string>

namespace industrial_bus {

struct Destination {
    std::string service_name;
    std::string instance_id;

    bool empty() const noexcept { return service_name.empty(); }
};

}  // namespace industrial_bus
