#include "industrial_bus/bootstrap/endpoint_record.h"
#include "industrial_bus/bootstrap/hub_launcher.h"
#include "industrial_bus/bootstrap/hub_lock.h"
#include "industrial_bus/bootstrap/runtime_paths.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <limits.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

class TempDirectory {
public:
    TempDirectory() {
        char pattern[] = "/tmp/industrial-bus-bootstrap-XXXXXX";
        const char* result = ::mkdtemp(pattern);
        if (result == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = result;
        if (::chmod(path_.c_str(), 0700) != 0) throw std::runtime_error("chmod failed");
    }
    ~TempDirectory() {
        const std::string child = path_ + "/industrial-bus";
        ::unlink((child + "/commhub.lock").c_str());
        ::unlink((child + "/endpoint.json").c_str());
        ::unlink((child + "/auth.token").c_str());
        ::unlink((child + "/bad-link").c_str());
        ::rmdir(child.c_str());
        ::rmdir(path_.c_str());
    }
    const std::string& path() const noexcept { return path_; }
private:
    std::string path_;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void requireThrows(Function function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

void happyPath() {
    TempDirectory temp;
    const auto paths = industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir(temp.path());
    require(paths.endpoint().rfind("ipc://", 0) == 0, "IPC endpoint missing scheme");

    struct stat status {};
    require(::stat(paths.runtimeDir().c_str(), &status) == 0, "runtime directory missing");
    require((status.st_mode & 0777) == 0700, "runtime directory mode is not 0700");

    auto lock = industrial_bus::bootstrap::HubLock::tryAcquire(paths);
    require(lock.has_value(), "first lock acquisition failed");
    require(!industrial_bus::bootstrap::HubLock::tryAcquire(paths).has_value(), "second lock acquisition succeeded");
    require((::fcntl(lock->descriptor(), F_GETFD) & FD_CLOEXEC) != 0, "lock does not start CLOEXEC");
    lock->makeInheritableForExec();
    require((::fcntl(lock->descriptor(), F_GETFD) & FD_CLOEXEC) == 0, "lock did not become inheritable");
    const pid_t child = ::fork();
    require(child >= 0, "fork inherited-lock test failed");
    if (child == 0) {
        const std::string descriptor = std::to_string(lock->descriptor());
        ::execl("/proc/self/exe", "industrial_bus_bootstrap_test",
                "--adopt-lock", temp.path().c_str(), descriptor.c_str(), nullptr);
        ::_exit(127);
    }
    int child_status = 0;
    require(::waitpid(child, &child_status, 0) == child,
            "wait inherited-lock child failed");
    require(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
            "inherited-lock child failed to adopt descriptor");
    lock->setCloseOnExec();

    const int inherited_fd = ::dup(lock->descriptor());
    require(inherited_fd >= 0, "duplicate lock fd failed");
    {
        auto inherited =
            industrial_bus::bootstrap::HubLock::adoptInherited(paths, inherited_fd);
        require((::fcntl(inherited.descriptor(), F_GETFD) & FD_CLOEXEC) != 0,
                "adopted lock did not restore CLOEXEC");
    }

    const std::string wrong_path = temp.path() + "/wrong.lock";
    const int wrong_fd = ::open(wrong_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    require(wrong_fd >= 0, "create wrong inherited lock failed");
    requireThrows(
        [&] {
            auto wrong =
                industrial_bus::bootstrap::HubLock::adoptInherited(paths, wrong_fd);
            (void)wrong;
        },
        "descriptor for another inode was accepted");
    ::close(wrong_fd);
    ::unlink(wrong_path.c_str());

    const auto token = industrial_bus::bootstrap::generateAuthToken();
    require(token.size() == 64, "token size is wrong");
    industrial_bus::bootstrap::writeAuthToken(paths, token);
    require(industrial_bus::bootstrap::readAuthToken(paths) == token, "token round trip failed");

    industrial_bus::bootstrap::EndpointRecord expected{
        paths.endpoint(), static_cast<std::int64_t>(::getpid()), "hub-test-instance", 1, 1700000000};
    industrial_bus::bootstrap::writeEndpointRecord(paths, expected);
    const auto actual = industrial_bus::bootstrap::readEndpointRecord(paths);
    require(actual.endpoint == expected.endpoint && actual.pid == expected.pid &&
            actual.hub_instance_id == expected.hub_instance_id &&
            actual.protocol_version == expected.protocol_version &&
            actual.start_time == expected.start_time, "endpoint record round trip failed");

    require(::stat(paths.endpointRecordPath().c_str(), &status) == 0 &&
            (status.st_mode & 0777) == 0600, "endpoint mode is not 0600");
    require(::stat(paths.tokenPath().c_str(), &status) == 0 &&
            (status.st_mode & 0777) == 0600, "token mode is not 0600");
}

void rejectsUnsafePathsAndFiles() {
    requireThrows([] { industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir("relative"); },
                  "relative XDG_RUNTIME_DIR accepted");
    TempDirectory temp;
    require(::chmod(temp.path().c_str(), 0755) == 0, "test chmod failed");
    requireThrows([&] { industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir(temp.path()); },
                  "unsafe XDG_RUNTIME_DIR mode accepted");
    require(::chmod(temp.path().c_str(), 0700) == 0, "test chmod restore failed");
    const auto paths = industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir(temp.path());

    const std::string target = temp.path() + "/token-target";
    const int target_fd = ::open(target.c_str(), O_CREAT | O_WRONLY, 0600);
    require(target_fd >= 0, "test target creation failed");
    ::close(target_fd);
    require(::symlink(target.c_str(), paths.tokenPath().c_str()) == 0, "test symlink creation failed");
    requireThrows([&] { (void)industrial_bus::bootstrap::readAuthToken(paths); },
                  "symlink token accepted");
    ::unlink(paths.tokenPath().c_str());
    ::unlink(target.c_str());

    industrial_bus::bootstrap::writeAuthToken(paths, industrial_bus::bootstrap::generateAuthToken());
    require(::chmod(paths.tokenPath().c_str(), 0644) == 0, "test chmod failed");
    requireThrows([&] { (void)industrial_bus::bootstrap::readAuthToken(paths); },
                  "overly broad token mode accepted");
}

void rejectsLongSocketPath() {
    TempDirectory temp;
    std::string child = temp.path() + "/" + std::string(90, 'x');
    require(::mkdir(child.c_str(), 0700) == 0, "long test directory creation failed");
    requireThrows([&] { industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir(child); },
                  "overlong Unix socket path accepted");
    ::rmdir((child + "/industrial-bus").c_str());
    ::rmdir(child.c_str());
}

std::string currentExecutable() {
    std::string path(PATH_MAX, '\0');
    const auto length = ::readlink("/proc/self/exe", path.data(), path.size());
    require(length > 0 && static_cast<std::size_t>(length) < path.size(),
            "read current executable path failed");
    path.resize(static_cast<std::size_t>(length));
    return path;
}

void launcherTransfersLockWithoutGap() {
    TempDirectory temp;
    require(::setenv("XDG_RUNTIME_DIR", temp.path().c_str(), 1) == 0,
            "set launcher XDG_RUNTIME_DIR failed");
    const auto paths = industrial_bus::bootstrap::RuntimePaths::fromEnvironment();
    auto lock = industrial_bus::bootstrap::HubLock::tryAcquire(paths);
    require(lock.has_value(), "launcher failed to acquire startup lock");
    const auto child = industrial_bus::bootstrap::launchCommHub(
        paths, std::move(*lock), currentExecutable());
    require(child > 0, "launcher returned invalid child pid");
    require(!industrial_bus::bootstrap::HubLock::tryAcquire(paths).has_value(),
            "startup lock had a parent-to-child ownership gap");

    std::optional<industrial_bus::bootstrap::HubLock> reacquired;
    for (int attempt = 0; attempt < 100 && !reacquired; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        reacquired = industrial_bus::bootstrap::HubLock::tryAcquire(paths);
    }
    require(reacquired.has_value(), "startup lock was not released after child exit");
    ::unsetenv("XDG_RUNTIME_DIR");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::string(argv[1]).rfind("--startup-lock-fd=", 0) == 0) {
        try {
            const auto paths =
                industrial_bus::bootstrap::RuntimePaths::fromEnvironment();
            const auto separator = std::string(argv[1]).find('=');
            auto lock = industrial_bus::bootstrap::HubLock::adoptInherited(
                paths, std::stoi(std::string(argv[1]).substr(separator + 1)));
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return lock.descriptor() >= 0 ? 0 : 1;
        } catch (const std::exception&) {
            return 1;
        }
    }
    if (argc == 4 && std::string(argv[1]) == "--adopt-lock") {
        try {
            const auto paths =
                industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir(argv[2]);
            auto lock = industrial_bus::bootstrap::HubLock::adoptInherited(
                paths, std::stoi(argv[3]));
            return lock.descriptor() >= 0 ? 0 : 1;
        } catch (const std::exception&) {
            return 1;
        }
    }
    try {
        happyPath();
        rejectsUnsafePathsAndFiles();
        rejectsLongSocketPath();
        launcherTransfersLockWithoutGap();
        std::cout << "bootstrap tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bootstrap test failed: " << error.what() << '\n';
        return 1;
    }
}
