#pragma once

#include "industrial_bus/client_options.h"

#include <memory>

namespace industrial_bus::client {

class IRuntimeAdapter;

std::shared_ptr<IRuntimeAdapter> makeLinuxRuntimeAdapter(
    const ClientOptions& options);

}  // namespace industrial_bus::client
