#include "industrial_bus/bootstrap/endpoint_record.h"
#include "industrial_bus/bootstrap/hub_launcher.h"
#include "industrial_bus/bootstrap/hub_lock.h"
#include "industrial_bus/bootstrap/runtime_paths.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fcntl.h>
#include <limits.h>
#include <optional>
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
        if (result == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = result;
        if (::chmod(path_.c_str(), 0700) != 0) {
            throw std::runtime_error("chmod failed");
        }
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

std::string currentExecutable() {
    std::string path(PATH_MAX, '\0');
    const auto length = ::readlink("/proc/self/exe", path.data(), path.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= path.size()) {
        throw std::runtime_error("read current executable path failed");
    }
    path.resize(static_cast<std::size_t>(length));
    return path;
}

TEST(BootstrapTest, SecuresRuntimeFilesAndTransfersLockAcrossExec) {
    TempDirectory temp;
    const auto paths =
        industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir(temp.path());
    EXPECT_EQ(paths.endpoint().rfind("ipc://", 0), 0U);

    struct stat status {};
    ASSERT_EQ(::stat(paths.runtimeDir().c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0700);

    auto lock = industrial_bus::bootstrap::HubLock::tryAcquire(paths);
    ASSERT_TRUE(lock.has_value());
    EXPECT_FALSE(industrial_bus::bootstrap::HubLock::tryAcquire(paths).has_value());
    EXPECT_NE(::fcntl(lock->descriptor(), F_GETFD) & FD_CLOEXEC, 0);
    lock->makeInheritableForExec();
    EXPECT_EQ(::fcntl(lock->descriptor(), F_GETFD) & FD_CLOEXEC, 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        const std::string descriptor = std::to_string(lock->descriptor());
        ::execl("/proc/self/exe", "industrial_bus_bootstrap_test",
                "--adopt-lock", temp.path().c_str(), descriptor.c_str(), nullptr);
        ::_exit(127);
    }
    int child_status = 0;
    ASSERT_EQ(::waitpid(child, &child_status, 0), child);
    ASSERT_TRUE(WIFEXITED(child_status));
    EXPECT_EQ(WEXITSTATUS(child_status), 0);
    lock->setCloseOnExec();

    const int inherited_fd = ::dup(lock->descriptor());
    ASSERT_GE(inherited_fd, 0);
    {
        auto inherited =
            industrial_bus::bootstrap::HubLock::adoptInherited(paths, inherited_fd);
        EXPECT_NE(::fcntl(inherited.descriptor(), F_GETFD) & FD_CLOEXEC, 0);
    }

    const std::string wrong_path = temp.path() + "/wrong.lock";
    const int wrong_fd =
        ::open(wrong_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(wrong_fd, 0);
    EXPECT_THROW(
        (void)industrial_bus::bootstrap::HubLock::adoptInherited(paths, wrong_fd),
        std::exception);
    ::close(wrong_fd);
    ::unlink(wrong_path.c_str());

    const auto token = industrial_bus::bootstrap::generateAuthToken();
    EXPECT_EQ(token.size(), 64U);
    industrial_bus::bootstrap::writeAuthToken(paths, token);
    EXPECT_EQ(industrial_bus::bootstrap::readAuthToken(paths), token);

    industrial_bus::bootstrap::EndpointRecord expected{
        paths.endpoint(), static_cast<std::int64_t>(::getpid()),
        "hub-test-instance", 1, 1700000000};
    industrial_bus::bootstrap::writeEndpointRecord(paths, expected);
    const auto actual = industrial_bus::bootstrap::readEndpointRecord(paths);
    EXPECT_EQ(actual.endpoint, expected.endpoint);
    EXPECT_EQ(actual.pid, expected.pid);
    EXPECT_EQ(actual.hub_instance_id, expected.hub_instance_id);
    EXPECT_EQ(actual.protocol_version, expected.protocol_version);
    EXPECT_EQ(actual.start_time, expected.start_time);

    ASSERT_EQ(::stat(paths.endpointRecordPath().c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
    ASSERT_EQ(::stat(paths.tokenPath().c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
}

TEST(BootstrapTest, RejectsUnsafePathsAndFiles) {
    EXPECT_THROW(
        industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir("relative"),
        std::exception);
    TempDirectory temp;
    ASSERT_EQ(::chmod(temp.path().c_str(), 0755), 0);
    EXPECT_THROW(
        industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir(temp.path()),
        std::exception);
    ASSERT_EQ(::chmod(temp.path().c_str(), 0700), 0);
    const auto paths =
        industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir(temp.path());

    const std::string target = temp.path() + "/token-target";
    const int target_fd = ::open(target.c_str(), O_CREAT | O_WRONLY, 0600);
    ASSERT_GE(target_fd, 0);
    ::close(target_fd);
    ASSERT_EQ(::symlink(target.c_str(), paths.tokenPath().c_str()), 0);
    EXPECT_THROW(industrial_bus::bootstrap::readAuthToken(paths), std::exception);
    ::unlink(paths.tokenPath().c_str());
    ::unlink(target.c_str());

    industrial_bus::bootstrap::writeAuthToken(
        paths, industrial_bus::bootstrap::generateAuthToken());
    ASSERT_EQ(::chmod(paths.tokenPath().c_str(), 0644), 0);
    EXPECT_THROW(industrial_bus::bootstrap::readAuthToken(paths), std::exception);
}

TEST(BootstrapTest, RejectsOverlongUnixSocketPath) {
    TempDirectory temp;
    const std::string child = temp.path() + "/" + std::string(90, 'x');
    ASSERT_EQ(::mkdir(child.c_str(), 0700), 0);
    EXPECT_THROW(
        industrial_bus::bootstrap::RuntimePaths::fromXdgRuntimeDir(child),
        std::exception);
    ::rmdir((child + "/industrial-bus").c_str());
    ::rmdir(child.c_str());
}

TEST(BootstrapTest, LauncherTransfersLockWithoutOwnershipGap) {
    TempDirectory temp;
    ASSERT_EQ(::setenv("XDG_RUNTIME_DIR", temp.path().c_str(), 1), 0);
    const auto paths = industrial_bus::bootstrap::RuntimePaths::fromEnvironment();
    auto lock = industrial_bus::bootstrap::HubLock::tryAcquire(paths);
    ASSERT_TRUE(lock.has_value());
    const auto child = industrial_bus::bootstrap::launchCommHub(
        paths, std::move(*lock), currentExecutable());
    ASSERT_GT(child, 0);
    EXPECT_FALSE(industrial_bus::bootstrap::HubLock::tryAcquire(paths).has_value());

    std::optional<industrial_bus::bootstrap::HubLock> reacquired;
    for (int attempt = 0; attempt < 100 && !reacquired; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        reacquired = industrial_bus::bootstrap::HubLock::tryAcquire(paths);
    }
    EXPECT_TRUE(reacquired.has_value());
    ::unsetenv("XDG_RUNTIME_DIR");
}

int runChildMode(int argc, char** argv) {
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
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    const int child_result = runChildMode(argc, argv);
    if (child_result >= 0) {
        return child_result;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
