#include "industrial_bus/bootstrap/hub_lock.h"

#include "internal.h"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
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
