// SPDX-License-Identifier: GPL-2.0
// Mock kernel interfaces: this test never samples or pages out real processes.
#include "IdleBitmapScan.h"
#include "ProcMaps.h"
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <iostream>
#include <linux/kernel-page-flags.h>
#include <map>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <type_traits>
#include <unistd.h>

static_assert(!std::is_copy_constructible<IdleBitmapScan>::value, "scanner owns descriptors");
static_assert(!std::is_copy_assignable<IdleBitmapScan>::value, "scanner owns descriptors");

enum FileKind { PIDFD, PAGEMAP, FLAGS, BITMAP };
enum Fault { NONE, CLOCK_ERROR, WAIT_ERROR, WRITE_ERROR, READ_ERROR,
             SHORT_WRITE, SHORT_READ, NO_SAMPLES };
static std::map<int, FileKind> open_fds;
static int next_fd = 100;
static unsigned closed_fds;
static int opens_until_failure = -1;
static Fault fault;
static int interruptions;
static std::vector<timespec> deadlines;
static std::vector<unsigned long> advised;
static long advise_result = 4096;
static bool real_time;
static volatile sig_atomic_t signals_received;
static uint64_t pfn_stride = 1;
static uint64_t bitmap_value;
static bool fake_timing;
static uint64_t wall_time_ns, cpu_time_ns;

static int open_mock(FileKind kind)
{
  if (opens_until_failure == 0) {
    errno = EMFILE;
    return -1;
  }
  if (opens_until_failure > 0)
    --opens_until_failure;
  int fd = next_fd++;
  open_fds.emplace(fd, kind);
  return fd;
}

std::vector<proc_maps_entry> ProcMaps::load(pid_t pid)
{
  assert(pid == 42);
  proc_maps_entry vma = {};
  vma.start = 0x1000;
  vma.end = 0x4000;
  vma.read = vma.write = true;
  return {vma};
}

extern "C" int __wrap_open(const char *path, int, ...)
{
  if (!strcmp(path, "/proc/42/pagemap"))
    return open_mock(PAGEMAP);
  if (!strcmp(path, "/proc/kpageflags"))
    return open_mock(FLAGS);
  assert(!strcmp(path, "/sys/kernel/mm/page_idle/bitmap"));
  return open_mock(BITMAP);
}

extern "C" int __wrap_close(int fd)
{
  assert(open_fds.erase(fd) == 1); // Also catches double-close.
  ++closed_fds;
  return 0;
}

extern "C" long __wrap_sysconf(int name)
{
  assert(name == _SC_PAGESIZE);
  return 4096;
}

extern "C" int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
  assert(count == 1 && timeout == 0);
  assert(open_fds.at(fds[0].fd) == PIDFD);
  return 0;
}

extern "C" ssize_t __wrap_pread(int fd, void *buf, size_t count, off_t offset)
{
  assert(count >= 8 && count <= 4096 && count % 8 == 0 && offset % 8 == 0);
  FileKind kind = open_fds.at(fd);
  uint64_t value = 0;
  if (kind == PAGEMAP)
    value = (1ULL << 63) | (100 + pfn_stride * offset / 8);
  else if (kind == FLAGS)
    value = fault == NO_SAMPLES ? 0 : (1ULL << KPF_ANON) | (1ULL << KPF_LRU);
  else {
    assert(kind == BITMAP);
    if (fake_timing) {
      wall_time_ns += 11000000;
      cpu_time_ns += 5000000;
    }
    if (fault == READ_ERROR) {
      errno = EIO;
      return -1;
    }
    if (fault == SHORT_READ)
      return 4;
    value = bitmap_value;
  }
  if (kind != BITMAP) assert(count == 8);
  for (size_t i = 0; i < count; i += 8)
    memcpy(static_cast<char *>(buf) + i, &value, 8);
  return count;
}

extern "C" ssize_t __wrap_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
  assert(open_fds.at(fd) == BITMAP && count >= 8 && count <= 4096 && count % 8 == 0 && offset % 8 == 0);
  // Every written bit must belong to one of the mock target pages.
  for (size_t i = 0; i < count / 8; ++i) {
    uint64_t mask = static_cast<const uint64_t *>(buf)[i];
    uint64_t allowed = 0;
    for (unsigned page = 1; page <= 3; ++page) {
      uint64_t pfn = 100 + pfn_stride * page;
      if (pfn / 64 == static_cast<uint64_t>(offset / 8) + i)
        allowed |= 1ULL << (pfn % 64);
    }
    assert(mask && !(mask & ~allowed));
  }
  if (fake_timing) {
    wall_time_ns += 7000000;
    cpu_time_ns += 3000000;
  }
  if (fault == WRITE_ERROR) {
    errno = EIO;
    return -1;
  }
  if (fault == SHORT_WRITE)
    return 4;
  return count;
}

extern "C" int __real_clock_gettime(clockid_t, struct timespec *);
extern "C" int __real_clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);

extern "C" int __wrap_clock_gettime(clockid_t clock, struct timespec *now)
{
  assert(clock == CLOCK_MONOTONIC || clock == CLOCK_THREAD_CPUTIME_ID);
  if (real_time)
    return __real_clock_gettime(clock, now);
  if (fault == CLOCK_ERROR) {
    errno = EIO;
    return -1;
  }
  uint64_t ns = fake_timing ? (clock == CLOCK_MONOTONIC ? wall_time_ns : cpu_time_ns)
                           : 10900000000ULL;
  *now = {static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
  return 0;
}

extern "C" int __wrap_clock_nanosleep(clockid_t clock, int flags,
                                      const struct timespec *deadline, struct timespec *remaining)
{
  assert(clock == CLOCK_MONOTONIC && flags == TIMER_ABSTIME && !remaining);
  deadlines.push_back(*deadline);
  if (real_time)
    return __real_clock_nanosleep(clock, flags, deadline, remaining);
  // Deliberately differ from the return value: clock_nanosleep doesn't use errno.
  errno = EIO;
  if (interruptions > 0) {
    --interruptions;
    return EINTR;
  }
  if (fault == WAIT_ERROR)
    return EINVAL;
  if (fake_timing)
    wall_time_ns = static_cast<uint64_t>(deadline->tv_sec) * 1000000000 + deadline->tv_nsec;
  return 0;
}

extern "C" long __wrap_syscall(long number, ...)
{
  va_list args;
  va_start(args, number);
  if (number == SYS_pidfd_open) {
    assert(va_arg(args, int) == 42 && va_arg(args, int) == 0);
    va_end(args);
    return open_mock(PIDFD);
  }
  assert(number == SYS_process_madvise);
  assert(open_fds.at(va_arg(args, int)) == PIDFD);
  const struct iovec *iov = va_arg(args, struct iovec *);
  assert(va_arg(args, int) == 1);
  assert(va_arg(args, int) == MADV_PAGEOUT && va_arg(args, int) == 0);
  va_end(args);
  assert(iov->iov_len == 4096);
  advised.push_back(reinterpret_cast<unsigned long>(iov->iov_base));
  errno = EIO;
  return advise_result;
}

static void alarm_handler(int)
{
  ++signals_received;
}

static std::map<std::string, std::map<std::string, uint64_t>> read_stats(const std::string& path)
{
  std::ifstream in(path);
  std::string line, field;
  assert(std::getline(in, line));
  std::istringstream header(line);
  std::vector<std::string> columns;
  while (std::getline(header, field, '\t'))
    columns.push_back(field);
  std::map<std::string, std::map<std::string, uint64_t>> rows;
  while (std::getline(in, line)) {
    std::istringstream row(line);
    std::string label;
    assert(std::getline(row, label, '\t'));
    for (size_t i = 1; i < columns.size(); ++i) {
      assert(std::getline(row, field, '\t'));
      rows[label][columns[i]] = std::stoull(field);
    }
    assert(!std::getline(row, field, '\t'));
  }
  return rows;
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  std::string directory = argv[1];
  std::string addresses = directory + "/addresses";
  std::string output = directory + "/samples.tsv";
  std::string stats_output = directory + "/bitmap-stats.tsv";
  AddrSequence refs;
  std::vector<void *> candidates = {reinterpret_cast<void *>(0x1000),
                                    reinterpret_cast<void *>(0x2000)};
  auto write_address = [&](const char *address) {
    std::ofstream file(addresses);
    file << address << '\n';
  };

  {
    IdleBitmapScan scanner;
    auto scan = [&](int rounds = 1, double interval = 1.25) {
      return scanner.scan(42, 0x1000, 0x4000, addresses, rounds, interval, output, refs, stats_output);
    };
    auto check_failed_scan = [&]() {
      assert(open_fds.empty() && refs.empty());
      advised.clear();
      assert(scanner.pageout(candidates, 2) == -EINVAL);
      assert(advised.empty());
    };

    check_failed_scan();
    write_address("0x1000");
    interruptions = 3;
    assert(scan() == 0 && refs.size() == 1 && open_fds.size() == 4);
    assert(deadlines.size() == 4);
    for (const auto& deadline : deadlines)
      assert(deadline.tv_sec == 12 && deadline.tv_nsec == 150000000);
    std::cout << "PASS: repeated EINTR retries the same normalized deadline\n";

    write_address("0x2000");
    assert(scan() == 0 && refs.size() == 1 && open_fds.size() == 4);
    assert(closed_fds == 4);
    assert(scanner.pageout(candidates, 1) == 0);
    assert(advised == std::vector<unsigned long>{0x2000});
    assert(scan(0) == -EINVAL);
    check_failed_scan();
    std::cout << "PASS: replacement scan closes old fds and drops old candidates\n";

    write_address("0x1000\n0x2000");
    assert(scan() == 0);
    advised.clear();
    assert(scanner.pageout(candidates, 0) == -EINVAL && advised.empty());
    for (long result : {4096L, -1L, 0L, 2048L}) {
      advise_result = result;
      advised.clear();
      assert(scanner.pageout(candidates, 1) == (result == 4096 ? 0 : -EIO));
      assert(advised == std::vector<unsigned long>{0x1000});
    }
    advise_result = 4096;
    advised.clear();
    assert(scanner.pageout(candidates, 2) == 0);
    assert((advised == std::vector<unsigned long>{0x1000, 0x2000}));
    write_address("0x2000");
    std::cout << "PASS: pageout limit counts successful, failed and short attempts; skips do not consume it\n";

    for (int fail_after = 0; fail_after < 4; ++fail_after) {
      opens_until_failure = fail_after;
      assert(scan() == -EINVAL);
      check_failed_scan();
      opens_until_failure = -1;
      assert(scan() == 0 && open_fds.size() == 4);
    }
    std::cout << "PASS: failure at each open releases partial state and permits retry\n";

    for (Fault injected : {CLOCK_ERROR, WAIT_ERROR, WRITE_ERROR, READ_ERROR,
                           SHORT_WRITE, SHORT_READ, NO_SAMPLES}) {
      fault = injected;
      assert(scan() == (fault == NO_SAMPLES ? -ENODATA : -EINVAL));
      check_failed_scan();
      fault = NONE;
      assert(scan() == 0 && open_fds.size() == 4);
    }
    assert(scanner.scan(42, 0x1000, 0x4000, addresses, 1, 1.25, "/dev/full", refs) == -EINVAL);
    check_failed_scan();
    std::cout << "PASS: clock, wait, bitmap and output failures invalidate pageout state\n";

    write_address("0x1000\n0x2000");
    fake_timing = true;
    wall_time_ns = 10900000000ULL;
    cpu_time_ns = 0;
    assert(scan(2) == 0);
    auto stats = read_stats(stats_output);
    assert(stats.size() == 3);
    for (const auto& label : {"1", "2"}) {
      const auto& row = stats.at(label);
      assert(row.at("complete") == 1);
      assert(row.at("eligible_hvas") == 2 && row.at("unique_pfns") == 2);
      assert(row.at("bitmap_words") == 1 && row.at("planned_pfn_positions") == 64);
      assert(row.at("write_calls") == 1 && row.at("write_bytes") == 8);
      assert(row.at("read_calls") == 1 && row.at("read_bytes") == 8);
      assert(row.at("write_wall_ns") == 7000000 && row.at("write_cpu_ns") == 3000000);
      assert(row.at("read_wall_ns") == 11000000 && row.at("read_cpu_ns") == 5000000);
      assert(row.at("wait_wall_ns") == 1250000000);
      assert(row.at("valid_hvas") == 2 && row.at("accessed_hvas") == 2 && row.at("idle_hvas") == 0);
    }
    assert(stats.at("total").at("read_wall_ns") == 22000000);
    assert(stats.at("total").at("write_calls") == 2);
    std::cout << "PASS: per-round and total timing excludes observation wait and non-bitmap reads\n";

    pfn_stride = 64;
    bitmap_value = ~0ULL;
    assert(scan() == 0);
    stats = read_stats(stats_output);
    assert(stats.at("1").at("bitmap_words") == 2);
    assert(stats.at("1").at("planned_pfn_positions") == 128);
    assert(stats.at("1").at("read_calls") == 2);
    assert(stats.at("1").at("read_bytes") == 16);
    assert(stats.at("1").at("idle_hvas") == 2);
    write_address("0x1000\n0x2000\n0x3000");
    for (unsigned batch : {8U, 16U, 64U, 512U, 4096U}) {
      assert(scanner.scan(42, 0x1000, 0x4000, addresses, 1, 1.25, output, refs, stats_output, batch) == 0);
      stats = read_stats(stats_output);
      const auto& row = stats.at("1");
      unsigned calls = batch == 8 ? 3 : batch == 16 ? 2 : 1;
      assert(row.at("read_calls") == calls && row.at("write_calls") == calls);
      assert(row.at("read_bytes") == 24 && row.at("write_bytes") == 24);
      assert(row.at("planned_pfn_positions") == 192 && row.at("idle_hvas") == 3);
      assert(row.at("batch_bytes") == batch);
    }
    pfn_stride = 128; // Gaps must not be bridged, even with a large buffer.
    assert(scanner.scan(42, 0x1000, 0x4000, addresses, 1, 1.25, output, refs, stats_output, 4096) == 0);
    stats = read_stats(stats_output);
    assert(stats.at("1").at("read_calls") == 3 && stats.at("1").at("read_bytes") == 24);
    for (unsigned batch : {0U, 7U, 9U, 4104U}) {
      assert(scanner.scan(42, 0x1000, 0x4000, addresses, 1, 1.25, output, refs, stats_output, batch) == -EINVAL);
      check_failed_scan();
    }
    pfn_stride = 64;
    for (Fault injected : {SHORT_WRITE, SHORT_READ}) {
      fault = injected;
      assert(scanner.scan(42, 0x1000, 0x4000, addresses, 1, 1.25, output, refs, stats_output, 4096) == -EINVAL);
      check_failed_scan();
    }
    fault = NONE;
    write_address("0x1000\n0x2000");
    std::cout << "PASS: batching preserves coverage and masks, splits at bounds/gaps, rejects invalid sizes and short I/O\n";
    pfn_stride = 0; // Two HVAs alias the same PFN.
    assert(scan() == 0);
    stats = read_stats(stats_output);
    assert(stats.at("1").at("unique_pfns") == 1);
    assert(stats.at("1").at("eligible_hvas") == 2);
    assert(stats.at("1").at("read_calls") == 1);
    std::cout << "PASS: sparse words, duplicate PFNs, and idle target counts\n";
    pfn_stride = 1;
    bitmap_value = 0;

    for (Fault injected : {WRITE_ERROR, READ_ERROR, SHORT_WRITE, SHORT_READ}) {
      fault = injected;
      assert(scan() == -EINVAL);
      check_failed_scan();
      stats = read_stats(stats_output);
      const auto& row = stats.at("1");
      assert(row.at("complete") == 0 && stats.at("total").at("complete") == 0);
      bool writing = fault == WRITE_ERROR || fault == SHORT_WRITE;
      std::string prefix = writing ? "write_" : "read_";
      bool short_io = fault == SHORT_WRITE || fault == SHORT_READ;
      assert(row.at(prefix + "calls") == 1);
      assert(row.at(prefix + "bytes") == (short_io ? 4 : 0));
      assert(row.at(prefix + "short_calls") == (short_io ? 1 : 0));
      assert(row.at(prefix + "errors") == (short_io ? 0 : 1));
      assert(row.at(prefix + "wall_ns") == (writing ? 7000000 : 11000000));
      assert(row.at("read_calls") == (writing ? 0 : 1));
    }
    fault = NONE;
    assert(scanner.scan(42, 0x1000, 0x4000, addresses, 1, 1.25, output, refs, "/dev/full") == -EINVAL);
    check_failed_scan();
    fault = NO_SAMPLES;
    assert(scan() == -ENODATA);
    stats = read_stats(stats_output);
    assert(stats.at("total").at("read_calls") == 0);
    assert(stats.at("total").at("write_calls") == 0);
    assert(stats.at("total").at("unique_pfns") == 0);
    assert(stats.at("total").at("complete") == 0);
    fault = NONE;
    fake_timing = false;
    std::cout << "PASS: I/O failures, short transfers, empty scans and statistics output failure\n";

    real_time = true;
    deadlines.clear();
    struct sigaction action = {}, previous;
    action.sa_handler = alarm_handler;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGALRM, &action, &previous) == 0);
    struct itimerval timer = {};
    timer.it_interval.tv_usec = timer.it_value.tv_usec = 10000;
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    struct timespec start, end;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    assert(scan(1, 0.075) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    timer = {};
    assert(setitimer(ITIMER_REAL, &timer, nullptr) == 0);
    assert(sigaction(SIGALRM, &previous, nullptr) == 0);
    double elapsed = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
    assert(elapsed >= 0.075 && signals_received > 0 && deadlines.size() > 1);
    for (const auto& deadline : deadlines)
      assert(deadline.tv_sec == deadlines[0].tv_sec && deadline.tv_nsec == deadlines[0].tv_nsec);
    std::cout << "PASS: real periodic signals do not shorten the 75 ms sample window\n";
  }
  assert(open_fds.empty());
  std::cout << "PASS: destructor closes all remaining descriptors exactly once\n";
}
