/* SPDX-License-Identifier: GPL-2.0 */

#include "IdleSnapshot.h"
#include "IdleBitmapScan.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

void write_idle_snapshot(const IdleSample& sample,
                         const std::string& output_path)
{
  const std::string pattern = output_path + ".tmp.XXXXXX";
  std::vector<char> temporary_path(pattern.begin(), pattern.end());
  temporary_path.push_back('\0');

  const int fd = mkstemp(temporary_path.data());
  if (fd < 0)
    throw std::system_error(errno, std::generic_category(),
                            "create temporary snapshot");

  /*
   * The path buffer outlives this guard. Unlink on every exit, including
   * exceptions. After a successful rename, the temporary name is absent.
   */
  struct RemoveTemporaryFile
  {
    const char* path;

    ~RemoveTemporaryFile()
    {
      unlink(path);
    }
  };
  const RemoveTemporaryFile cleanup = {temporary_path.data()};

  /*
   * Transfer descriptor ownership to the FILE stream, then to unique_ptr.
   * fclose is checked explicitly before publication; exceptions also close it.
   */
  std::unique_ptr<FILE, decltype(&fclose)> file(fdopen(fd, "w"), &fclose);
  if (!file) {
    const int error = errno;
    close(fd);
    throw std::system_error(error, std::generic_category(), "fdopen snapshot");
  }

  if (fprintf(file.get(), "%-18s\trefs\tobserved\n", "hva") < 0)
    throw std::system_error(errno, std::generic_category(),
                            "write snapshot header");

  for (const IdlePage& page : sample.pages) {
    if (fprintf(file.get(), "0x%016" PRIx64 "\t%u\t%u\n",
                page.hva, page.refs, static_cast<unsigned>(page.observed)) < 0)
      throw std::system_error(errno, std::generic_category(),
                              "write snapshot row");
  }

  /* Allow a viewer running as another user to read the published snapshot. */
  if (fchmod(fd, 0644) != 0)
    throw std::system_error(errno, std::generic_category(),
                            "set snapshot permissions");

  if (fclose(file.release()) != 0)
    throw std::system_error(errno, std::generic_category(), "close snapshot");

  /*
   * Readers see either the old file or the complete new TSV. Without fsync,
   * this atomic replacement does not guarantee persistence after a crash.
   */
  if (rename(temporary_path.data(), output_path.c_str()) != 0)
    throw std::system_error(errno, std::generic_category(), "publish snapshot");
}
