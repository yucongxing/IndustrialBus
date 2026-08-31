#pragma once

#include <string>

namespace industrial_bus::bootstrap {

// Validated paths for the per-user CommHub runtime. Construction validates
// XDG_RUNTIME_DIR and creates/validates its industrial-bus child directory.
class RuntimePaths {
public:
    static RuntimePaths fromEnvironment();
    static RuntimePaths fromXdgRuntimeDir(const std::string& xdg_runtime_dir);

    const std::string& xdgRuntimeDir() const noexcept { return xdg_runtime_dir_; }
    const std::string& runtimeDir() const noexcept { return runtime_dir_; }
    const std::string& socketPath() const noexcept { return socket_path_; }
    const std::string& lockPath() const noexcept { return lock_path_; }
    const std::string& endpointRecordPath() const noexcept { return endpoint_path_; }
    const std::string& tokenPath() const noexcept { return token_path_; }
    const std::string& endpoint() const noexcept { return endpoint_; }

    // Opens and revalidates the private runtime directory. The returned fd has
    // FD_CLOEXEC set and is owned by the caller.
    int openRuntimeDirectory() const;

private:
    explicit RuntimePaths(std::string xdg_runtime_dir);

    std::string xdg_runtime_dir_;
    std::string runtime_dir_;
    std::string socket_path_;
    std::string lock_path_;
    std::string endpoint_path_;
    std::string token_path_;
    std::string endpoint_;
};

}  // namespace industrial_bus::bootstrap
