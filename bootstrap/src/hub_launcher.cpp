#include "industrial_bus/bootstrap/hub_launcher.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace industrial_bus::bootstrap {
namespace {

constexpr int kChildLockDescriptor = 198;

class SpawnActions {
public:
    SpawnActions() {
        const int status = ::posix_spawn_file_actions_init(&actions_);
        if (status != 0) {
            throw std::runtime_error(std::string("initialize spawn actions: ") +
                                     std::strerror(status));
        }
        initialized_ = true;
    }
    ~SpawnActions() {
        if (initialized_) {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
    }
    posix_spawn_file_actions_t* get() noexcept { return &actions_; }

private:
    posix_spawn_file_actions_t actions_{};
    bool initialized_{false};
};

void validateExecutable(const std::string& executable) {
    if (executable.empty() || executable.front() != '/' ||
        executable.size() >= PATH_MAX) {
        throw std::invalid_argument(
            "CommHub executable must be an absolute, bounded path");
    }
    struct stat status {};
    if (::lstat(executable.c_str(), &status) != 0) {
        throw std::runtime_error(std::string("inspect CommHub executable: ") +
                                 std::strerror(errno));
    }
    if (!S_ISREG(status.st_mode) ||
        (status.st_uid != ::geteuid() && status.st_uid != 0) ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        ::access(executable.c_str(), X_OK) != 0) {
        throw std::invalid_argument(
            "CommHub executable must be trusted, regular and executable");
    }
}

void validateEnvironment(const RuntimePaths& paths) {
    const char* runtime = ::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || paths.xdgRuntimeDir() != runtime) {
        throw std::invalid_argument(
            "XDG_RUNTIME_DIR must match the validated RuntimePaths");
    }
}

void reap(pid_t child) noexcept {
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
}

}  // namespace

pid_t launchCommHub(const RuntimePaths& paths,
                    HubLock lock,
                    const std::string& executable) {
    validateExecutable(executable);
    validateEnvironment(paths);
    if (lock.descriptor() < 0) {
        throw std::invalid_argument("CommHub launch requires an owned startup lock");
    }

    const int descriptor_flags = ::fcntl(lock.descriptor(), F_GETFD);
    if (descriptor_flags < 0 ||
        ::fcntl(lock.descriptor(), F_SETFD,
                descriptor_flags | FD_CLOEXEC) != 0) {
        throw std::runtime_error(std::string("secure startup lock fd: ") +
                                 std::strerror(errno));
    }

    int source_fd = lock.descriptor();
    int duplicated_fd = -1;
    if (source_fd == kChildLockDescriptor) {
        duplicated_fd = ::fcntl(source_fd, F_DUPFD_CLOEXEC,
                                kChildLockDescriptor + 1);
        if (duplicated_fd < 0) {
            throw std::runtime_error(std::string("duplicate startup lock fd: ") +
                                     std::strerror(errno));
        }
        source_fd = duplicated_fd;
    }

    SpawnActions actions;
    const int action_status = ::posix_spawn_file_actions_adddup2(
        actions.get(), source_fd, kChildLockDescriptor);
    if (action_status != 0) {
        if (duplicated_fd >= 0) {
            ::close(duplicated_fd);
        }
        throw std::runtime_error(std::string("configure startup lock fd: ") +
                                 std::strerror(action_status));
    }

    const std::string lock_argument =
        "--startup-lock-fd=" + std::to_string(kChildLockDescriptor);
    char* arguments[] = {const_cast<char*>(executable.c_str()),
                         const_cast<char*>(lock_argument.c_str()), nullptr};
    pid_t child = -1;
    const int spawn_status = ::posix_spawn(
        &child, executable.c_str(), actions.get(), nullptr, arguments, environ);
    if (duplicated_fd >= 0) {
        ::close(duplicated_fd);
    }
    if (spawn_status != 0) {
        throw std::runtime_error(std::string("spawn CommHub: ") +
                                 std::strerror(spawn_status));
    }

    try {
        std::thread(reap, child).detach();
    } catch (const std::exception&) {
        ::kill(child, SIGTERM);
        reap(child);
        throw std::runtime_error("failed to create CommHub child reaper");
    }
    return child;
}

}  // namespace industrial_bus::bootstrap
