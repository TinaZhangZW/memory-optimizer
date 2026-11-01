/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IDLE_PAGE_IO_H
#define IDLE_PAGE_IO_H

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace idle_bitmap {

constexpr size_t WORD_BYTES = sizeof(uint64_t);

enum class IOOperation {
  READ,
  WRITE
};

[[noreturn]] inline void throw_system_error(const std::string& operation)
{
  throw std::system_error(errno, std::generic_category(), operation);
}

// Own one descriptor. Copying is forbidden to prevent double close.
class FileDescriptor
{
public:
  explicit FileDescriptor(int fd, const char* operation) : fd_(fd)
  {
    if (fd_ < 0)
      throw_system_error(operation);
  }

  ~FileDescriptor() noexcept
  {
    close(fd_);
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  int get() const noexcept
  {
    return fd_;
  }

private:
  int fd_;
};

/*
 * offset is in bytes; count is the number of 64-bit entries.
 * Retry interrupted system calls. Throw on I/O errors or short transfers.
 */
inline void transfer_words(int fd, IOOperation operation, uint64_t offset,
                    uint64_t* words, size_t count)
{
  const size_t bytes = count * WORD_BYTES;
  ssize_t transferred;

  do {
    transferred = operation == IOOperation::WRITE
        ? pwrite(fd, words, bytes, static_cast<off_t>(offset))
        : pread(fd, words, bytes, static_cast<off_t>(offset));
  } while (transferred < 0 && errno == EINTR);

  if (transferred < 0) {
    throw_system_error(operation == IOOperation::WRITE
        ? "pwrite idle bitmap" : "pread page metadata/bitmap");
  }

  if (static_cast<size_t>(transferred) != bytes)
    throw std::runtime_error("short page metadata/bitmap transfer");
}

} // namespace idle_bitmap

#endif
