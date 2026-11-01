/* SPDX-License-Identifier: GPL-2.0 */
#include "idle-page-fixture.h"

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <linux/kernel-page-flags.h>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

namespace idle_bitmap_test {

FixtureState state;

PageFixture::PageFixture(size_t page_count)
    : page_count_(page_count),
      memory_(mmap(nullptr, page_count_ * PAGE_SIZE_BYTES,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))
{
  assert(memory_ != MAP_FAILED);
  reset();
}

PageFixture::~PageFixture()
{
  munmap(memory_, page_count_ * PAGE_SIZE_BYTES);
}

void PageFixture::reset()
{
  state = FixtureState();
  state.range_start = reinterpret_cast<uint64_t>(memory_);
  state.page_count = page_count_;
}

} // namespace idle_bitmap_test

namespace {
using namespace idle_bitmap_test;
int pagemap_fd = -1;
int flags_fd = -1;
int bitmap_fd = -1;
int lock_fd = -1;
} // namespace

extern "C" int __real_open(const char*, int, ...);
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" int __real_flock(int, int);

extern "C" int __wrap_flock(int fd, int operation)
{
  if (fd != lock_fd)
    return __real_flock(fd, operation);

  if (state.lock_busy) {
    errno = EWOULDBLOCK;
    return -1;
  }

  return 0;
}

extern "C" int __wrap_open(const char* path, int flags, ...)
{
  const std::string name(path);
  int* fd = nullptr;

  if (name == "/proc/" + std::to_string(getpid()) + "/pagemap")
    fd = &pagemap_fd;
  if (name == "/proc/kpageflags")
    fd = &flags_fd;
  if (name == "/sys/kernel/mm/page_idle/bitmap")
    fd = &bitmap_fd;
  if (name.find("/run/lock/memory-optimizer-idle-") == 0)
    fd = &lock_fd;

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
  if (fd != pagemap_fd && fd != flags_fd && fd != bitmap_fd)
    return __real_pread(fd, data, size, offset);

  assert(size > 0 && size <= PAGE_SIZE_BYTES && size % WORD_BYTES == 0);
  assert(offset >= 0 && offset % WORD_BYTES == 0);

  unsigned call;
  unsigned short_call;
  if (fd == pagemap_fd) {
    call = ++state.pagemap_reads;
    short_call = state.short_pagemap_read_call;
  } else if (fd == flags_fd) {
    call = ++state.flags_reads;
    state.flag_words += size / WORD_BYTES;
    short_call = state.short_flags_read_call;
  } else {
    call = ++state.bitmap_reads;
    short_call = state.short_bitmap_read_call;
  }

  if (state.io_error) {
    errno = EIO;
    return -1;
  }

  const bool short_transfer = state.short_read ||
      (short_call != 0 && call == short_call);
  const size_t copied_words = size / WORD_BYTES - (short_transfer ? 1 : 0);
  auto words = static_cast<uint64_t*>(data);

  for (size_t i = 0; i < copied_words; ++i) {
    const uint64_t index = static_cast<uint64_t>(offset) / WORD_BYTES + i;

    if (fd == pagemap_fd) {
      assert(index >= state.range_start / PAGE_SIZE_BYTES &&
             index < state.range_start / PAGE_SIZE_BYTES + state.page_count);

      const size_t page = index - state.range_start / PAGE_SIZE_BYTES;
      if (page == 0)
        ++state.pagemap_passes;

      uint64_t pfn = FIRST_PFN + page;
      if (!state.page_pfns.empty()) {
        assert(state.page_pfns.size() == state.page_count);
        pfn = state.page_pfns[page];
      } else if (page == state.page_count - 1) {
        if (state.sparse_pfns)
          pfn = DISTANT_PFN;
        if (state.alias_last_page)
          pfn = FIRST_PFN;
        if (state.adjacent_words)
          pfn = FIRST_PFN + WORD_BITS;
      }
      words[i] = pfn == idle_bitmap::NO_PFN ? 0 : PRESENT_BIT | pfn;

      if (state.use_default_page_states) {
        if (page == 2)
          words[i] = SWAPPED_BIT;
        if (page == 4 && state.pagemap_passes > 1)
          words[i] += 100;
        if (page == 5 && state.pagemap_passes > 1)
          words[i] = 0;
      }
    } else if (fd == flags_fd) {
      words[i] = (1ULL << KPF_ANON) | (1ULL << KPF_LRU);

      if (state.use_default_page_states && index == FIRST_PFN + 3)
        words[i] |= 1ULL << KPF_THP;
      if (state.lose_lru && state.pagemap_passes > 1 && index == FIRST_PFN + 6)
        words[i] &= ~(1ULL << KPF_LRU);
    } else {
      assert(state.written_bitmap.count(index));
      words[i] = state.written_bitmap.at(index);

      if (index == (FIRST_PFN + 1) / WORD_BITS)
        words[i] &= ~(1ULL << ((FIRST_PFN + 1) % WORD_BITS));
    }
  }

  return static_cast<ssize_t>(copied_words * WORD_BYTES);
}

extern "C" ssize_t __wrap_pwrite(int fd, const void* data, size_t size,
                                 off_t offset)
{
  if (fd != bitmap_fd)
    return __real_pwrite(fd, data, size, offset);

  assert(size > 0 && size <= PAGE_SIZE_BYTES && size % WORD_BYTES == 0);
  assert(offset >= 0 && offset % WORD_BYTES == 0);

  const unsigned call = ++state.bitmap_writes;
  const bool short_transfer = state.short_write ||
      (state.short_bitmap_write_call != 0 &&
       call == state.short_bitmap_write_call);
  const size_t copied_words = size / WORD_BYTES - (short_transfer ? 1 : 0);
  const auto words = static_cast<const uint64_t*>(data);

  for (size_t i = 0; i < copied_words; ++i)
    state.written_bitmap[static_cast<uint64_t>(offset) / WORD_BYTES + i] =
        words[i];

  return static_cast<ssize_t>(copied_words * WORD_BYTES);
}
