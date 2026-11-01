/* SPDX-License-Identifier: GPL-2.0 */
#include "idle-page-fixture.h"

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <linux/kernel-page-flags.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace idle_bitmap_test {

FixtureState state;

PageFixture::PageFixture()
    : memory_(mmap(nullptr, TEST_PAGE_COUNT * PAGE_SIZE_BYTES,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))
{
  assert(memory_ != MAP_FAILED);
  state.range_start = reinterpret_cast<uint64_t>(memory_);
  reset();
}

PageFixture::~PageFixture()
{
  munmap(memory_, TEST_PAGE_COUNT * PAGE_SIZE_BYTES);
}

void PageFixture::reset()
{
  const uint64_t range_start = state.range_start;
  state = FixtureState();
  state.range_start = range_start;
}

} // namespace idle_bitmap_test

namespace {
using namespace idle_bitmap_test;
int pagemap_fd = -1;
int flags_fd = -1;

} // namespace

extern "C" int __real_open(const char*, int, ...);
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);

extern "C" int __wrap_open(const char* path, int flags, ...)
{
  const std::string name(path);
  int* fd = nullptr;

  if (name == "/proc/" + std::to_string(getpid()) + "/pagemap")
    fd = &pagemap_fd;
  if (name == "/proc/kpageflags")
    fd = &flags_fd;

  if (fd)
    return *fd = __real_open("/dev/null", O_RDWR);

  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
  }

  return __real_open(path, flags, mode);
}

extern "C" ssize_t __wrap_pread(int fd, void* data, size_t size, off_t offset)
{
  if (fd != pagemap_fd && fd != flags_fd)
    return __real_pread(fd, data, size, offset);

  if (state.io_error) {
    errno = EIO;
    return -1;
  }

  if (state.short_read)
    return size - 1;

  auto words = static_cast<uint64_t*>(data);

  if (fd == pagemap_fd)
    ++state.pagemap_reads;
  if (fd == flags_fd) {
    ++state.flags_reads;
    state.flag_words += size / WORD_BYTES;
  }

  for (size_t i = 0; i < size / WORD_BYTES; ++i) {
    const uint64_t index = offset / WORD_BYTES + i;

    if (fd == pagemap_fd) {
      assert(index >= state.range_start / PAGE_SIZE_BYTES &&
             index < state.range_start / PAGE_SIZE_BYTES + TEST_PAGE_COUNT);

      const uint64_t page = index - state.range_start / PAGE_SIZE_BYTES;
      if (page == 0)
        ++state.pagemap_passes;
      words[i] = PRESENT_BIT | (FIRST_PFN + page);
      if (state.sparse_pfns && page == TEST_PAGE_COUNT - 1)
        words[i] = PRESENT_BIT | DISTANT_PFN;

      if (page == 2)
        words[i] = SWAPPED_BIT; // swapped
    } else if (fd == flags_fd) {
      words[i] = (1ULL << KPF_ANON) | (1ULL << KPF_LRU);

      if (index == FIRST_PFN + 3)
        words[i] |= 1ULL << KPF_THP;
    }
  }

  return size;
}
