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
enum Fault { NONE, CLOCK_ERROR, WAIT_ERROR, WRITE_ERROR, NO_SAMPLES };
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
  assert(count == 8 && offset % 8 == 0);
  FileKind kind = open_fds.at(fd);
  uint64_t value = 0;
  if (kind == PAGEMAP)
    value = (1ULL << 63) | (100 + offset / 8);
  else if (kind == FLAGS)
    value = fault == NO_SAMPLES ? 0 : (1ULL << KPF_ANON) | (1ULL << KPF_LRU);
  else
    assert(kind == BITMAP); // Every sampled page is accessed (idle bit = 0).
  memcpy(buf, &value, 8);
  return 8;
}

extern "C" ssize_t __wrap_pwrite(int fd, const void *, size_t count, off_t offset)
{
  assert(open_fds.at(fd) == BITMAP && count == 8 && offset % 8 == 0);
  if (fault == WRITE_ERROR) {
    errno = EIO;
    return -1;
  }
  return 8;
}

extern "C" int __real_clock_gettime(clockid_t, struct timespec *);
extern "C" int __real_clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);

extern "C" int __wrap_clock_gettime(clockid_t clock, struct timespec *now)
{
  assert(clock == CLOCK_MONOTONIC);
  if (real_time)
    return __real_clock_gettime(clock, now);
  if (fault == CLOCK_ERROR) {
    errno = EIO;
    return -1;
  }
  *now = {10, 900000000};
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
  return fault == WAIT_ERROR ? EINVAL : 0;
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

int main(int argc, char **argv)
{
  assert(argc == 2);
  std::string directory = argv[1];
  std::string addresses = directory + "/addresses";
  std::string output = directory + "/samples.tsv";
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
      return scanner.scan(42, 0x1000, 0x4000, addresses, rounds, interval, output, refs);
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

    for (Fault injected : {CLOCK_ERROR, WAIT_ERROR, WRITE_ERROR, NO_SAMPLES}) {
      fault = injected;
      assert(scan() == (fault == NO_SAMPLES ? -ENODATA : -EINVAL));
      check_failed_scan();
      fault = NONE;
      assert(scan() == 0 && open_fds.size() == 4);
    }
    assert(scanner.scan(42, 0x1000, 0x4000, addresses, 1, 1.25, "/dev/full", refs) == -EINVAL);
    check_failed_scan();
    std::cout << "PASS: clock, wait, bitmap and output failures invalidate pageout state\n";

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
