#pragma once

#include "industrial_bus/bootstrap/runtime_paths.h"

#include <optional>

namespace industrial_bus::bootstrap {

// Owns both the commhub.lock descriptor and its exclusive flock.
class HubLock {
public:
    static std::optional<HubLock> tryAcquire(const RuntimePaths& paths);

    // Adopts a descriptor inherited across exec. On success this object owns fd.
    // The descriptor must reference this RuntimePaths' private commhub.lock file.
    static HubLock adoptInherited(const RuntimePaths& paths, int fd);

    ~HubLock();
    HubLock(HubLock&& other) noexcept;
    HubLock& operator=(HubLock&& other) noexcept;
    HubLock(const HubLock&) = delete;
    HubLock& operator=(const HubLock&) = delete;

    int descriptor() const noexcept { return fd_; }

    // Clear CLOEXEC immediately before spawning/executing the Hub so the lock
    // remains held across exec. Call setCloseOnExec() if exec is abandoned.
    void makeInheritableForExec();
    void setCloseOnExec();

private:
    explicit HubLock(int fd) noexcept : fd_(fd) {}
    int fd_{-1};
};

}  // namespace industrial_bus::bootstrap
