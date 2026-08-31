#pragma once

#include <string>

namespace industrial_bus::bootstrap::internal {

[[noreturn]] void throwErrno(const std::string& operation);
void validatePrivateDirectoryFd(int fd, const std::string& description);
void validatePrivateRegularFileFd(int fd, const std::string& description);
int openRuntimeDirectory(const std::string& path);
void closeNoThrow(int fd) noexcept;
void atomicWriteFile(int directory_fd,
                     const std::string& final_name,
                     const std::string& contents);
std::string readSecureFile(int directory_fd,
                           const std::string& name,
                           std::size_t maximum_size);

}  // namespace industrial_bus::bootstrap::internal
