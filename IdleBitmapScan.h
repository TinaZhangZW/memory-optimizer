// SPDX-License-Identifier: GPL-2.0
#ifndef IDLE_BITMAP_SCAN_H
#define IDLE_BITMAP_SCAN_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "AddrSequence.h"

class BitmapWorkerPool;

// Explicit, bounded HVA allowlist for a private anonymous guest RAM mapping.
class IdleBitmapScan {
public:
  IdleBitmapScan() = default;
  IdleBitmapScan(const IdleBitmapScan&) = delete;
  IdleBitmapScan& operator=(const IdleBitmapScan&) = delete;
  ~IdleBitmapScan();

  // A new scan replaces the previous samples; a failed scan disables pageout.
  // stats_output optionally receives per-round bitmap I/O statistics as TSV.
  int scan(int pid, unsigned long start, unsigned long end,
           const std::string& address_file, int rounds, double interval,
           const std::string& output, AddrSequence& refs,
           const std::string& stats_output = "", unsigned batch_bytes = 8, unsigned threads = 1,
           unsigned max_pages = 262144);
  // Count each attempted 4 KiB page, including failed syscalls; skips do not count.
  int pageout(const std::vector<void *>& addresses, unsigned long max_pageout_pages);

private:
  struct PageSample {
    uint64_t pfn = 0;
    unsigned accessed_rounds = 0;
    unsigned valid_rounds = 0;
  };

  struct BitmapIOStats {
    uint64_t calls = 0;
    uint64_t bytes = 0;
    uint64_t short_calls = 0;
    uint64_t errors = 0;
    uint64_t wall_ns = 0;
    uint64_t cpu_ns = 0;
  };

  struct RoundStats {
    bool complete = false;
    uint64_t active_workers = 0;
    uint64_t requested_hvas = 0;
    uint64_t eligible_hvas = 0;
    uint64_t unique_pfns = 0;
    uint64_t bitmap_words = 0;
    uint64_t valid_hvas = 0;
    uint64_t idle_hvas = 0;
    uint64_t accessed_hvas = 0;
    uint64_t wait_wall_ns = 0;
    BitmapIOStats write, read;
  };

  void reset();
  void sample_round(const std::vector<unsigned long>& addresses,
                    std::vector<PageSample>& samples, double interval, RoundStats& stats,
                    BitmapWorkerPool& workers);
  void bitmap_io(int fd, bool write, std::vector<uint64_t>& words, uint64_t offset, BitmapIOStats& stats);
  void report_stats(const std::vector<RoundStats>& rounds, const std::string& output,
                    int scan_rc) const;
  unsigned save_results(const std::vector<unsigned long>& addresses,
                        const std::vector<PageSample>& samples, int rounds,
                        const std::string& output, AddrSequence& refs);
  bool eligible(uint64_t pfn) const;
  uint64_t mapping(unsigned long address) const;
  bool mapping_matches(unsigned long address, uint64_t pfn) const;
  bool alive() const;

  unsigned bitmap_batch_bytes = 8;
  unsigned bitmap_threads = 1;
  int pidfd = -1;
  int pagemap_fd = -1;
  int flags_fd = -1;
  std::vector<int> bitmap_fds;
  std::map<unsigned long, uint64_t> sampled_pfns;
};
#endif
