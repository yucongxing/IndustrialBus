#include "industrial_bus/bootstrap/runtime_paths.h"

#include "internal.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace industrial_bus::bootstrap {
namespace {

constexpr const char* kRuntimeSubdirectory = "industrial-bus";

class ScopedFd {
public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() { internal::closeNoThrow(fd_); }
    int get() const noexcept { return fd_; }
private:
    int fd_;
};

void validateAbsolutePath(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("XDG_RUNTIME_DIR is empty");
    }
    if (path.front() != '/') {
        throw std::invalid_argument("XDG_RUNTIME_DIR must be an absolute path");
    }
    if (path.size() > 1 && path.back() == '/') {
        throw std::invalid_argument("XDG_RUNTIME_DIR must not have a trailing slash");
    }
    if (path.find('\0') != std::string::npos) {
        throw std::invalid_argument("XDG_RUNTIME_DIR contains a NUL byte");
    }
}

}  // namespace

RuntimePaths RuntimePaths::fromEnvironment() {
    const char* value = std::getenv("XDG_RUNTIME_DIR");
    if (value == nullptr) {
        throw std::runtime_error("XDG_RUNTIME_DIR is not set");
    }
    return fromXdgRuntimeDir(value);
}

RuntimePaths RuntimePaths::fromXdgRuntimeDir(const std::string& xdg_runtime_dir) {
    validateAbsolutePath(xdg_runtime_dir);

    ScopedFd parent(internal::openRuntimeDirectory(xdg_runtime_dir));
    if (::mkdirat(parent.get(), kRuntimeSubdirectory, 0700) != 0 && errno != EEXIST) {
        internal::throwErrno("create IndustrialBus runtime directory");
    }

    const int child_fd = ::openat(parent.get(), kRuntimeSubdirectory,
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child_fd < 0) {
        internal::throwErrno("open IndustrialBus runtime directory");
    }
    ScopedFd child(child_fd);
    internal::validatePrivateDirectoryFd(child.get(), "IndustrialBus runtime directory");
    return RuntimePaths(xdg_runtime_dir);
}

RuntimePaths::RuntimePaths(std::string xdg_runtime_dir)
    : xdg_runtime_dir_(std::move(xdg_runtime_dir)),
      runtime_dir_(xdg_runtime_dir_ + "/" + kRuntimeSubdirectory),
      socket_path_(runtime_dir_ + "/commhub.sock"),
      lock_path_(runtime_dir_ + "/commhub.lock"),
      endpoint_path_(runtime_dir_ + "/endpoint.json"),
      token_path_(runtime_dir_ + "/auth.token"),
      endpoint_("ipc://" + socket_path_) {
    // sockaddr_un::sun_path is 108 bytes on Linux, including the terminating NUL.
    constexpr std::size_t kMaximumUnixSocketPath = 107;
    if (socket_path_.size() > kMaximumUnixSocketPath) {
        throw std::length_error("CommHub Unix socket path exceeds Linux sockaddr_un limit");
    }
}

int RuntimePaths::openRuntimeDirectory() const {
    return internal::openRuntimeDirectory(runtime_dir_);
}

}  // namespace industrial_bus::bootstrap
