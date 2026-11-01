/* SPDX-License-Identifier: GPL-2.0 */
#include "IdleBitmapScan.h"
#include "IdleSnapshot.h"

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

namespace {
unsigned write_calls = 0;
unsigned fail_write_call = 0;
bool fail_close = false;
bool fail_permissions = false;

mode_t file_permissions(const std::string& path)
{
  struct stat status;
  assert(stat(path.c_str(), &status) == 0);
  return status.st_mode & 0777;
}

std::string read_file(const std::string& path)
{
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void require_no_temporary_files(const std::string& directory)
{
  DIR* entries = opendir(directory.c_str());
  assert(entries != nullptr);
  while (const dirent* entry = readdir(entries))
    assert(std::string(entry->d_name).find(".tmp.") == std::string::npos);
  closedir(entries);
}
} // namespace

extern "C" int __real_fclose(FILE* stream);
extern "C" int __real_fchmod(int fd, mode_t mode);

extern "C" int __wrap_fchmod(int fd, mode_t mode)
{
  if (fail_permissions) {
    errno = EPERM;
    return -1;
  }
  return __real_fchmod(fd, mode);
}

extern "C" int __wrap_fclose(FILE* stream)
{
  const int result = __real_fclose(stream);
  if (fail_close) {
    errno = ENOSPC;
    return EOF;
  }
  return result;
}

extern "C" int __wrap_fprintf(FILE* stream, const char* format, ...)
{
  if (++write_calls == fail_write_call) {
    errno = ENOSPC;
    return -1;
  }

  va_list arguments;
  va_start(arguments, format);
  const int result = vfprintf(stream, format, arguments);
  va_end(arguments);
  return result;
}

int main(int argc, char* argv[])
{
  assert(argc == 2);
  const std::string directory = argv[1];
  const std::string output = directory + "/pages.tsv";
  IdleSample sample;
  sample.pages = {{0x1000, 0, true}, {0x2000, 1, true}, {0x3000, 0, false}};

  /* A restrictive umask must not prevent another user from reading output. */
  const mode_t previous_umask = umask(0077);
  write_idle_snapshot(sample, output);
  umask(previous_umask);
  const std::string expected = "hva" + std::string(15, ' ') +
      "\trefs\tobserved\n"
      "0x0000000000001000\t0\t1\n"
      "0x0000000000002000\t1\t1\n"
      "0x0000000000003000\t0\t0\n";
  assert(read_file(output) == expected);
  assert(file_permissions(output) == 0644);

  /* Replacement uses the declared mode rather than the old file's mode. */
  assert(chmod(output.c_str(), 0600) == 0);
  write_idle_snapshot(sample, output);
  assert(read_file(output) == expected);
  assert(file_permissions(output) == 0644);

  /* Failed writes, permission changes or close must preserve old output. */
  for (unsigned failure : {1U, 2U, 3U, 4U}) {
    std::ofstream(output) << "keep this snapshot\n";
    assert(chmod(output.c_str(), 0600) == 0);
    write_calls = 0;
    fail_write_call = failure < 3 ? failure : 0;
    fail_close = failure == 3;
    fail_permissions = failure == 4;
    bool failed = false;
    try {
      write_idle_snapshot(sample, output);
    } catch (const std::exception&) {
      failed = true;
    }
    assert(failed);
    assert(read_file(output) == "keep this snapshot\n");
    assert(file_permissions(output) == 0600);
    require_no_temporary_files(directory);
  }
  fail_write_call = 0;
  fail_close = false;
  fail_permissions = false;

  // Rename must fail when the destination is an existing directory.
  const std::string blocked = directory + "/blocked";
  assert(mkdir(blocked.c_str(), 0700) == 0);
  std::ofstream(blocked + "/keep") << "unchanged\n";
  bool failed = false;
  try {
    write_idle_snapshot(sample, blocked);
  } catch (const std::exception&) {
    failed = true;
  }
  assert(failed);
  assert(read_file(blocked + "/keep") == "unchanged\n");
  require_no_temporary_files(directory);
}
