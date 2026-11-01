/* SPDX-License-Identifier: GPL-2.0 */

#include "IdleBitmapScan.h"
#include "IdlePageIO.h"

#include <array>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <time.h>

namespace {

using idle_bitmap::FileDescriptor;
using idle_bitmap::IOOperation;
using idle_bitmap::NO_PFN;
using idle_bitmap::WORD_BYTES;
using idle_bitmap::read_eligible_pfns;
using idle_bitmap::throw_system_error;
using idle_bitmap::transfer_words;

constexpr unsigned WORD_BITS = 64;
constexpr uint64_t MAX_SCAN_BYTES = 64ULL * 1024 * 1024 * 1024;
constexpr uint64_t NS_PER_SECOND = 1000000000;
constexpr uint64_t NS_PER_MS = 1000000;
using BitmapWords = std::map<uint64_t, uint64_t>;

constexpr size_t IO_BATCH_WORDS = IDLE_BITMAP_PAGE_SIZE / WORD_BYTES;

uint64_t monotonic_ns()
{
  timespec timestamp;

  if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
    throw_system_error("clock_gettime");

  return static_cast<uint64_t>(timestamp.tv_sec) * NS_PER_SECOND +
         static_cast<uint64_t>(timestamp.tv_nsec);
}

void require_target_alive(int pidfd)
{
  pollfd target = {pidfd, POLLIN, 0};
  int status;

  do {
    status = poll(&target, 1, 0);
  } while (status < 0 && errno == EINTR);

  if (status < 0)
    throw_system_error("poll pidfd");

  if (status != 0)
    throw std::runtime_error("target process exited during sampling");
}

void transfer_bitmap_words(int bitmap_fd, IOOperation operation,
                           BitmapWords& bitmap)
{
  /*
   * Write masks set bits only for selected PFNs.
   * Reads return the entire bitmap word.
   */
  for (auto batch_begin = bitmap.begin(); batch_begin != bitmap.end();) {
    const uint64_t first_word = batch_begin->first;
    auto batch_end = batch_begin;
    std::array<uint64_t, IO_BATCH_WORDS> words;
    size_t count = 0;

    // Merge only adjacent bitmap words.
    while (batch_end != bitmap.end() && count < words.size() &&
           batch_end->first == first_word + count) {
      words[count++] = batch_end->second;
      ++batch_end;
    }

    transfer_words(bitmap_fd, operation, first_word * WORD_BYTES,
                   words.data(), count);

    for (size_t i = 0; batch_begin != batch_end; ++batch_begin, ++i) {
      if (operation == IOOperation::READ)
        batch_begin->second = words[i];
    }
  }
}

void wait_until(uint64_t deadline_ns)
{
  const timespec deadline = {
    static_cast<time_t>(deadline_ns / NS_PER_SECOND),
    static_cast<long>(deadline_ns % NS_PER_SECOND)
  };
  int status;

  // Retry against the same deadline so EINTR does not restart the interval.
  do {
    status = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                             &deadline, nullptr);
  } while (status == EINTR);

  // clock_nanosleep returns an error number instead of setting errno.
  if (status != 0) {
    errno = status;
    throw_system_error("observation wait");
  }
}

} // namespace

IdleSample sample_idle_pages(int pid, uint64_t start, uint64_t end,
                            unsigned window_ms)
{
  if (pid <= 0 || start >= end || start % IDLE_BITMAP_PAGE_SIZE != 0 ||
      end % IDLE_BITMAP_PAGE_SIZE != 0 ||
      end > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      window_ms == 0 || end - start > MAX_SCAN_BYTES)
    throw std::invalid_argument(
        "require PID, aligned range <= 64 GiB and nonzero window");

  if (sysconf(_SC_PAGESIZE) != static_cast<long>(IDLE_BITMAP_PAGE_SIZE))
    throw std::runtime_error("idle sampling requires 4 KiB base pages");

  // A pidfd identifies this process for exit checks; it does not prevent exit.
  FileDescriptor identity(static_cast<int>(syscall(SYS_pidfd_open, pid, 0)),
                          "pidfd_open");
  require_target_alive(identity.get());

  /*
   * Coordinate only callers using this per-PID lock. Other tools can still
   * modify the global idle bitmap.
   * Keep the lock file: unlinking it would allow callers to lock different
   * inodes for the same target. Closing the descriptor releases the flock.
   */
  const std::string lock_path =
      "/run/lock/memory-optimizer-idle-" + std::to_string(pid) + ".lock";
  FileDescriptor lock(open(lock_path.c_str(),
                           O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600),
                      lock_path.c_str());

  if (flock(lock.get(), LOCK_EX | LOCK_NB) != 0)
    throw_system_error("target already sampled or lock unavailable");

  const std::string pagemap_path = "/proc/" + std::to_string(pid) + "/pagemap";
  FileDescriptor pagemap(open(pagemap_path.c_str(), O_RDONLY | O_CLOEXEC),
                         pagemap_path.c_str());
  FileDescriptor flags(open("/proc/kpageflags", O_RDONLY | O_CLOEXEC),
                       "open /proc/kpageflags");
  FileDescriptor bitmap(open("/sys/kernel/mm/page_idle/bitmap",
                             O_RDWR | O_CLOEXEC),
                        "open idle bitmap");

  // Capture eligible PFNs before setting idle bits.
  const auto before = read_eligible_pfns(
      pid, pagemap.get(), flags.get(), start, end);
  require_target_alive(identity.get());

  BitmapWords words;
  for (const uint64_t pfn : before) {
    if (pfn != NO_PFN)
      words[pfn / WORD_BITS] |= 1ULL << (pfn % WORD_BITS);
  }

  IdleSample sample;
  sample.clear_start_ns = monotonic_ns();
  transfer_bitmap_words(bitmap.get(), IOOperation::WRITE, words);
  sample.clear_end_ns = monotonic_ns();

  // The requested wait starts after the last bitmap write, not scan setup.
  const uint64_t wait_ns = static_cast<uint64_t>(window_ms) * NS_PER_MS;
  wait_until(sample.clear_end_ns + wait_ns);

  sample.check_start_ns = monotonic_ns();
  transfer_bitmap_words(bitmap.get(), IOOperation::READ, words);
  sample.check_end_ns = monotonic_ns();

  /*
   * Recheck after reading idle bits. An entry is valid only if both
   * checks found an eligible page with the same PFN.
   */
  const auto after = read_eligible_pfns(
      pid, pagemap.get(), flags.get(), start, end);
  require_target_alive(identity.get());

  sample.pages.reserve(before.size());
  for (size_t i = 0; i < before.size(); ++i) {
    const uint64_t pfn = before[i];
    const bool observed = pfn != NO_PFN && pfn == after[i];
    unsigned refs = 0;

    if (observed) {
      const uint64_t idle_bits = words.at(pfn / WORD_BITS);
      const bool idle = (idle_bits & (1ULL << (pfn % WORD_BITS))) != 0;
      refs = idle ? 0 : 1;
    }

    sample.pages.push_back({start + i * IDLE_BITMAP_PAGE_SIZE, refs, observed});
  }

  return sample;
}
