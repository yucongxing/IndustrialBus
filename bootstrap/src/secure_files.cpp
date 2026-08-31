#include "internal.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace industrial_bus::bootstrap::internal {
namespace {

std::atomic<unsigned long> temporary_sequence{0};

void writeAll(int fd, const std::string& contents) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            throwErrno("write control file");
        }
        offset += static_cast<std::size_t>(count);
    }
}

}  // namespace

[[noreturn]] void throwErrno(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void closeNoThrow(int fd) noexcept {
    if (fd >= 0) {
        while (::close(fd) != 0 && errno == EINTR) {}
    }
}

void validatePrivateDirectoryFd(int fd, const std::string& description) {
    struct stat status {};
    if (::fstat(fd, &status) != 0) throwErrno("stat " + description);
    if (!S_ISDIR(status.st_mode)) throw std::runtime_error(description + " is not a directory");
    if (status.st_uid != ::geteuid()) throw std::runtime_error(description + " is not owned by current UID");
    if ((status.st_mode & 0777) != 0700) throw std::runtime_error(description + " permissions must be 0700");
}

void validatePrivateRegularFileFd(int fd, const std::string& description) {
    struct stat status {};
    if (::fstat(fd, &status) != 0) throwErrno("stat " + description);
    if (!S_ISREG(status.st_mode)) throw std::runtime_error(description + " is not a regular file");
    if (status.st_uid != ::geteuid()) throw std::runtime_error(description + " is not owned by current UID");
    if ((status.st_mode & 0777) != 0600) throw std::runtime_error(description + " permissions must be 0600");
}

int openRuntimeDirectory(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throwErrno("open runtime directory " + path);
    try {
        validatePrivateDirectoryFd(fd, path);
    } catch (...) {
        closeNoThrow(fd);
        throw;
    }
    return fd;
}

void atomicWriteFile(int directory_fd,
                     const std::string& final_name,
                     const std::string& contents) {
    const std::string temporary_name = "." + final_name + ".tmp." +
        std::to_string(static_cast<long long>(::getpid())) + "." +
        std::to_string(temporary_sequence.fetch_add(1));
    int fd = ::openat(directory_fd, temporary_name.c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) throwErrno("create temporary " + final_name);

    bool published = false;
    try {
        validatePrivateRegularFileFd(fd, temporary_name);
        writeAll(fd, contents);
        if (::fsync(fd) != 0) throwErrno("fsync temporary " + final_name);
        closeNoThrow(fd);
        fd = -1;
        if (::renameat(directory_fd, temporary_name.c_str(),
                       directory_fd, final_name.c_str()) != 0) {
            throwErrno("publish " + final_name);
        }
        published = true;
        if (::fsync(directory_fd) != 0) throwErrno("fsync runtime directory");
    } catch (...) {
        closeNoThrow(fd);
        if (!published) ::unlinkat(directory_fd, temporary_name.c_str(), 0);
        throw;
    }
}

std::string readSecureFile(int directory_fd,
                           const std::string& name,
                           std::size_t maximum_size) {
    const int fd = ::openat(directory_fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throwErrno("open " + name);
    try {
        validatePrivateRegularFileFd(fd, name);
        struct stat status {};
        if (::fstat(fd, &status) != 0) throwErrno("stat " + name);
        if (status.st_size < 0 || static_cast<unsigned long long>(status.st_size) > maximum_size) {
            throw std::runtime_error(name + " exceeds maximum allowed size");
        }
        std::string contents(static_cast<std::size_t>(status.st_size), '\0');
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const ssize_t count = ::read(fd, contents.data() + offset, contents.size() - offset);
            if (count < 0) {
                if (errno == EINTR) continue;
                throwErrno("read " + name);
            }
            if (count == 0) throw std::runtime_error(name + " changed while being read");
            offset += static_cast<std::size_t>(count);
        }
        closeNoThrow(fd);
        return contents;
    } catch (...) {
        closeNoThrow(fd);
        throw;
    }
}

}  // namespace industrial_bus::bootstrap::internal
