#include "industrial_bus/bootstrap/hub_lock.h"

#include "internal.h"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace industrial_bus::bootstrap {

std::optional<HubLock> HubLock::tryAcquire(const RuntimePaths& paths) {
    const int directory_fd = paths.openRuntimeDirectory();
    const int fd = ::openat(directory_fd, "commhub.lock",
                            O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    internal::closeNoThrow(directory_fd);
    if (fd < 0) internal::throwErrno("open commhub.lock");
    try {
        internal::validatePrivateRegularFileFd(fd, "commhub.lock");
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                internal::closeNoThrow(fd);
                return std::nullopt;
            }
            internal::throwErrno("lock commhub.lock");
        }
        return HubLock(fd);
    } catch (...) {
        internal::closeNoThrow(fd);
        throw;
    }
}

HubLock HubLock::adoptInherited(const RuntimePaths& paths, int fd) {
    if (fd < 0) {
        throw std::invalid_argument("inherited commhub.lock descriptor is invalid");
    }
    struct stat inherited {};
    if (::fstat(fd, &inherited) != 0) {
        internal::throwErrno("inspect inherited commhub.lock descriptor");
    }
    internal::validatePrivateRegularFileFd(fd, "inherited commhub.lock");

    const int directory_fd = paths.openRuntimeDirectory();
    struct stat expected {};
    const int inspected = ::fstatat(directory_fd, "commhub.lock", &expected,
                                    AT_SYMLINK_NOFOLLOW);
    const int inspect_errno = errno;
    internal::closeNoThrow(directory_fd);
    if (inspected != 0) {
        errno = inspect_errno;
        internal::throwErrno("inspect expected commhub.lock");
    }
    if (!S_ISREG(expected.st_mode) || expected.st_dev != inherited.st_dev ||
        expected.st_ino != inherited.st_ino) {
        throw std::invalid_argument(
            "inherited descriptor does not reference this runtime's commhub.lock");
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        internal::throwErrno("confirm inherited commhub.lock ownership");
    }
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        internal::throwErrno("restore CLOEXEC on inherited commhub.lock");
    }
    return HubLock(fd);
}

HubLock::~HubLock() { internal::closeNoThrow(fd_); }

HubLock::HubLock(HubLock&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

HubLock& HubLock::operator=(HubLock&& other) noexcept {
    if (this != &other) {
        internal::closeNoThrow(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void HubLock::makeInheritableForExec() {
    if (fd_ < 0) throw std::logic_error("HubLock is empty");
    const int flags = ::fcntl(fd_, F_GETFD);
    if (flags < 0) internal::throwErrno("read commhub.lock descriptor flags");
    if (::fcntl(fd_, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
        internal::throwErrno("make commhub.lock inheritable");
    }
}

void HubLock::setCloseOnExec() {
    if (fd_ < 0) throw std::logic_error("HubLock is empty");
    const int flags = ::fcntl(fd_, F_GETFD);
    if (flags < 0) internal::throwErrno("read commhub.lock descriptor flags");
    if (::fcntl(fd_, F_SETFD, flags | FD_CLOEXEC) != 0) {
        internal::throwErrno("set CLOEXEC on commhub.lock");
    }
}

}  // namespace industrial_bus::bootstrap
