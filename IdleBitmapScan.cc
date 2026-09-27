// SPDX-License-Identifier: GPL-2.0
#include "IdleBitmapScan.h"
#include "ProcMaps.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/kernel-page-flags.h>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static constexpr unsigned BASE_PAGE_SHIFT = 12;
static constexpr unsigned long BASE_PAGE_SIZE = 1UL << BASE_PAGE_SHIFT;
static constexpr unsigned WORD_BYTES = sizeof(uint64_t);
static constexpr unsigned WORD_BITS = WORD_BYTES * 8;
static constexpr unsigned MAX_SAMPLE_PAGES = 262144;
static constexpr int MAX_SAMPLE_ROUNDS = 255;
static constexpr double MAX_INTERVAL_SECONDS = 60;

static constexpr uint64_t PFN_MASK = (1ULL << 55) - 1;
static constexpr uint64_t PAGEMAP_PRESENT = 1ULL << 63;
static constexpr uint64_t PAGEMAP_SWAPPED = 1ULL << 62;
static constexpr uint64_t REJECTED_PAGE_FLAGS =
    (1ULL << KPF_COMPOUND_HEAD) | (1ULL << KPF_COMPOUND_TAIL) |
    (1ULL << KPF_HUGE) | (1ULL << KPF_THP) | (1ULL << KPF_KSM) |
    (1ULL << KPF_UNEVICTABLE) | (1ULL << KPF_HWPOISON) | (1ULL << KPF_NOPAGE);

static uint64_t read_word(int fd, uint64_t offset)
{
  uint64_t value;
  ssize_t n = pread(fd, &value, sizeof(value), offset);
  if (n != sizeof(value))
    throw std::runtime_error(n < 0 ? strerror(errno) : "short kernel bitmap read");
  return value;
}

static int open_file(const std::string& name, int mode)
{
  int fd = open(name.c_str(), mode | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error(name + ": " + strerror(errno));
  return fd;
}

static void close_file(int& fd)
{
  if (fd >= 0) {
    // On Linux, do not retry close() after EINTR: the descriptor is released.
    close(fd);
    fd = -1;
  }
}

static void wait_sample_interval(double interval)
{
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline))
    throw std::runtime_error("sample clock: " + std::string(strerror(errno)));

  const uint64_t ns_per_second = 1000000000;
  uint64_t interval_ns = static_cast<uint64_t>(std::ceil(interval * ns_per_second));
  deadline.tv_sec += interval_ns / ns_per_second;
  deadline.tv_nsec += interval_ns % ns_per_second;
  if (deadline.tv_nsec >= static_cast<long>(ns_per_second)) {
    ++deadline.tv_sec;
    deadline.tv_nsec -= ns_per_second;
  }

  // Retrying the same monotonic deadline preserves the window across signals.
  int error;
  do {
    error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
  } while (error == EINTR);

  // clock_nanosleep() returns the error number directly, without setting errno.
  if (error)
    throw std::runtime_error("sample wait: " + std::string(strerror(error)));
}

static void validate_scan_args(int pid, unsigned long start, unsigned long end,
                               int rounds, double interval)
{
  if (pid <= 0 || start >= end || start % BASE_PAGE_SIZE || end % BASE_PAGE_SIZE ||
      rounds < 1 || rounds > MAX_SAMPLE_ROUNDS || !std::isfinite(interval) ||
      interval <= 0 || interval > MAX_INTERVAL_SECONDS ||
      sysconf(_SC_PAGESIZE) != BASE_PAGE_SIZE)
    throw std::runtime_error("invalid PID, RAM range, rounds (1..255), or interval (0..60]");
}

static std::vector<unsigned long> read_addresses(const std::string& filename,
                                                unsigned long start,
                                                unsigned long end)
{
  std::ifstream input(filename);
  if (!input)
    throw std::runtime_error("cannot open address allowlist");

  std::vector<unsigned long> addresses;
  std::string token;

  while (input >> token) {
    size_t parsed;
    unsigned long address = std::stoul(token, &parsed, 0);

    if (parsed != token.size() || address % BASE_PAGE_SIZE ||
        address < start || address >= end)
      throw std::runtime_error("unaligned address or address outside RAM range");
    addresses.push_back(address);
    if (addresses.size() > MAX_SAMPLE_PAGES)
      throw std::runtime_error("POC allowlist limit is 262144 pages");
  }

  std::sort(addresses.begin(), addresses.end());
  if (addresses.empty() ||
      std::adjacent_find(addresses.begin(), addresses.end()) != addresses.end())
    throw std::runtime_error("empty or duplicate address allowlist");
  return addresses;
}

static void validate_mappings(int pid, const std::vector<unsigned long>& addresses)
{
  ProcMaps maps;
  const auto vmas = maps.load(pid);

  for (unsigned long address : addresses) {
    bool valid = false;

    for (const auto& vma : vmas) {
      if (vma.start <= address && address < vma.end)
        valid = vma.read && vma.write && !vma.exec && !vma.mayshare && !vma.ino;
    }
    if (!valid)
      throw std::runtime_error("allowlist is not private writable anonymous RAM");
  }
}

IdleBitmapScan::~IdleBitmapScan()
{
  reset();
}

void IdleBitmapScan::reset()
{
  close_file(pidfd);
  close_file(pagemap_fd);
  close_file(flags_fd);
  close_file(bitmap_fd);
  sampled_pfns.clear();
}

bool IdleBitmapScan::alive() const
{
  struct pollfd p = {pidfd, POLLIN, 0};
  return poll(&p, 1, 0) == 0;
}

uint64_t IdleBitmapScan::mapping(unsigned long address) const
{
  uint64_t entry = read_word(pagemap_fd, address / BASE_PAGE_SIZE * WORD_BYTES);

  return (entry & PAGEMAP_PRESENT) && !(entry & PAGEMAP_SWAPPED) ? entry & PFN_MASK : 0;
}

bool IdleBitmapScan::eligible(uint64_t pfn) const
{
  // Also reject PFNs hidden by insufficient privilege.
  if (!pfn)
    return false;

  uint64_t page_flags = read_word(flags_fd, pfn * WORD_BYTES);

  return (page_flags & (1ULL << KPF_LRU)) &&
         (page_flags & (1ULL << KPF_ANON)) && !(page_flags & REJECTED_PAGE_FLAGS);
}

bool IdleBitmapScan::mapping_matches(unsigned long address, uint64_t pfn) const
{
  return pfn && mapping(address) == pfn && eligible(pfn);
}

void IdleBitmapScan::sample_round(const std::vector<unsigned long>& addresses,
                                 std::vector<PageSample>& samples, double interval)
{
  if (!alive())
    throw std::runtime_error("target exited");

  // Each bitmap word covers WORD_BITS PFNs, independently of their HVAs.
  std::map<uint64_t, uint64_t> bitmap_words;

  for (size_t i = 0; i < addresses.size(); ++i) {
    auto& sample = samples[i];

    sample.pfn = mapping(addresses[i]);
    if (!eligible(sample.pfn)) {
      sample.pfn = 0;
      continue;
    }
    bitmap_words[sample.pfn / WORD_BITS] |= 1ULL << (sample.pfn % WORD_BITS);
  }

  // Writing a set bit establishes an idle baseline for the corresponding PFN.
  for (const auto& word : bitmap_words) {
    if (pwrite(bitmap_fd, &word.second, WORD_BYTES, word.first * WORD_BYTES) != WORD_BYTES)
      throw std::runtime_error("mark idle: " + std::string(strerror(errno)));
  }

  wait_sample_interval(interval);
  if (!alive())
    throw std::runtime_error("target exited during sampling");

  for (auto& word : bitmap_words)
    word.second = read_word(bitmap_fd, word.first * WORD_BYTES);

  for (size_t i = 0; i < addresses.size(); ++i) {
    auto& sample = samples[i];

    if (!mapping_matches(addresses[i], sample.pfn))
      continue;

    ++sample.valid_rounds;
    uint64_t idle_mask = 1ULL << (sample.pfn % WORD_BITS);
    bool idle = bitmap_words.at(sample.pfn / WORD_BITS) & idle_mask;

    // Count accessed windows, not the number of CPU memory accesses.
    if (!idle)
      ++sample.accessed_rounds;
  }
}

unsigned IdleBitmapScan::save_results(const std::vector<unsigned long>& addresses,
                                     const std::vector<PageSample>& samples,
                                     int rounds, const std::string& output,
                                     AddrSequence& refs)
{
  std::ofstream out(output);
  if (!out)
    throw std::runtime_error("cannot open per-page output");

  out << "hva\trefs\tvalid\trounds\teligible\n";
  refs.clear();
  refs.set_pageshift(BASE_PAGE_SHIFT);
  refs.rewind();

  unsigned complete = 0;
  for (size_t i = 0; i < addresses.size(); ++i) {
    const auto& sample = samples[i];
    bool fully_sampled = sample.valid_rounds == static_cast<unsigned>(rounds) &&
                         mapping_matches(addresses[i], sample.pfn);

    out << "0x" << std::hex << addresses[i] << std::dec << '\t'
        << sample.accessed_rounds << '\t' << sample.valid_rounds << '\t'
        << rounds << '\t' << fully_sampled << '\n';

    // Partial samples are retained in the TSV but excluded from page selection.
    if (!fully_sampled)
      continue;
    if (refs.inc_payload(addresses[i], sample.accessed_rounds))
      throw std::runtime_error("AddrSequence append failed");
    sampled_pfns[addresses[i]] = sample.pfn;
    ++complete;
  }

  out.close();
  if (!out)
    throw std::runtime_error("per-page output write failed");
  return complete;
}

int IdleBitmapScan::scan(int pid, unsigned long start, unsigned long end,
    const std::string& address_file, int rounds, double interval,
    const std::string& output, AddrSequence& refs)
{
  reset();
  refs.clear();

  try {
    validate_scan_args(pid, start, end, rounds, interval);

    pidfd = syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0)
      throw std::runtime_error(strerror(errno));
    pagemap_fd = open_file("/proc/" + std::to_string(pid) + "/pagemap", O_RDONLY);
    flags_fd = open_file("/proc/kpageflags", O_RDONLY);
    bitmap_fd = open_file("/sys/kernel/mm/page_idle/bitmap", O_RDWR);

    const auto addresses = read_addresses(address_file, start, end);
    validate_mappings(pid, addresses);

    std::vector<PageSample> samples(addresses.size());
    for (int round = 0; round < rounds; ++round)
      sample_round(addresses, samples, interval);

    unsigned complete = save_results(addresses, samples, rounds, output, refs);
    fprintf(stderr, "idle-bitmap: requested=%zu fully_sampled=%u skipped=%zu\n",
            addresses.size(), complete, addresses.size() - complete);
    if (!complete)
      reset();
    return complete ? 0 : -ENODATA;
  } catch (const std::exception& e) {
    fprintf(stderr, "idle-bitmap: %s\n", e.what());
    reset();
    refs.clear();
    return -EINVAL;
  }
}

int IdleBitmapScan::pageout(const std::vector<void *>& addresses, unsigned long max_pageout_pages)
{
  if (pidfd < 0 || !max_pageout_pages)
    return -EINVAL;

  unsigned long attempted_pages = 0;
  unsigned long submitted_bytes = 0;
  unsigned long skipped_pages = 0;
  unsigned long swapped_pages = 0;
  unsigned long error_pages = 0;
  std::vector<unsigned long> requested;

  try {
    for (void *ptr : addresses) {
      if (attempted_pages >= max_pageout_pages)
        break;
      if (!alive())
        throw std::runtime_error("target exited before pageout");

      auto address = reinterpret_cast<unsigned long>(ptr);
      auto found = sampled_pfns.find(address);

      // This recheck does not pin the mapping across process_madvise().
      if (found == sampled_pfns.end() || !mapping_matches(address, found->second)) {
        ++skipped_pages;
        continue;
      }

      struct iovec iov = {ptr, BASE_PAGE_SIZE};
      ++attempted_pages;
      ssize_t bytes_advised = syscall(SYS_process_madvise, pidfd, &iov, 1, MADV_PAGEOUT, 0);

      if (bytes_advised != BASE_PAGE_SIZE) {
        ++error_pages;
        if (bytes_advised < 0)
          fprintf(stderr, "pageout 0x%lx: %s\n", address, strerror(errno));
        continue;
      }
      submitted_bytes += bytes_advised;
      requested.push_back(address);
    }

    // A swapped PTE does not distinguish zswap from ordinary swap or swapcache.
    for (unsigned long address : requested) {
      uint64_t entry = read_word(pagemap_fd, address / BASE_PAGE_SIZE * WORD_BYTES);

      if (entry & PAGEMAP_SWAPPED)
        ++swapped_pages;
    }

    fprintf(stderr, "pageout: candidates=%zu attempted_pages=%lu submitted_bytes=%lu skipped=%lu errors=%lu swapped_after=%lu\n",
            addresses.size(), attempted_pages, submitted_bytes, skipped_pages, error_pages, swapped_pages);
    return error_pages ? -EIO : 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "pageout: %s\n", e.what());
    return -EIO;
  }
}
