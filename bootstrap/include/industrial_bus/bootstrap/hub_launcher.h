#pragma once

#include "industrial_bus/bootstrap/hub_lock.h"
#include "industrial_bus/bootstrap/runtime_paths.h"

#include <string>
#include <sys/types.h>

namespace industrial_bus::bootstrap {

// Spawns an absolute CommHub executable without a shell and transfers lock's
// open-file-description to it. The child is reaped asynchronously.
pid_t launchCommHub(const RuntimePaths& paths,
                    HubLock lock,
                    const std::string& executable);

}  // namespace industrial_bus::bootstrap
