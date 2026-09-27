// SPDX-License-Identifier: GPL-2.0
#ifndef IDLE_BITMAP_SCAN_H
#define IDLE_BITMAP_SCAN_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "AddrSequence.h"

// Explicit, bounded HVA allowlist for a private anonymous guest RAM mapping.
class IdleBitmapScan {
public:
  IdleBitmapScan() = default;
  IdleBitmapScan(const IdleBitmapScan&) = delete;
  IdleBitmapScan& operator=(const IdleBitmapScan&) = delete;
  ~IdleBitmapScan();

  // A new scan replaces the previous samples; a failed scan disables pageout.
  int scan(int pid, unsigned long start, unsigned long end,
           const std::string& address_file, int rounds, double interval,
           const std::string& output, AddrSequence& refs);
  // Count each attempted 4 KiB page, including failed syscalls; skips do not count.
  int pageout(const std::vector<void *>& addresses, unsigned long max_pageout_pages);

private:
  struct PageSample {
    uint64_t pfn = 0;
    unsigned accessed_rounds = 0;
    unsigned valid_rounds = 0;
  };

  void reset();
  void sample_round(const std::vector<unsigned long>& addresses,
                    std::vector<PageSample>& samples, double interval);
  unsigned save_results(const std::vector<unsigned long>& addresses,
                        const std::vector<PageSample>& samples, int rounds,
                        const std::string& output, AddrSequence& refs);
  bool eligible(uint64_t pfn) const;
  uint64_t mapping(unsigned long address) const;
  bool mapping_matches(unsigned long address, uint64_t pfn) const;
  bool alive() const;

  int pidfd = -1;
  int pagemap_fd = -1;
  int flags_fd = -1;
  int bitmap_fd = -1;
  std::map<unsigned long, uint64_t> sampled_pfns;
};
#endif
