/* SPDX-License-Identifier: GPL-2.0 */

#ifndef IDLE_BITMAP_SCAN_H
#define IDLE_BITMAP_SCAN_H

#include "IdlePageMap.h"

#include <cstdint>
#include <vector>

/*
 * Access result for one target-process virtual page.
 * Ignore refs unless observed is true.
 */
struct IdlePage
{
  uint64_t hva;
  // When observed is true: 1 means access detected, 0 means none detected.
  unsigned refs;
  // True when both checks found an eligible page with the same PFN.
  bool observed;
};

struct IdleSample
{
  /*
   * CLOCK_MONOTONIC timestamps in nanoseconds.
   * clear_* bracket writes that mark selected pages idle.
   * check_* bracket reads of idle bits.
   * Metadata lookup and validation are outside these I/O boundaries.
   */
  uint64_t clear_start_ns = 0;
  uint64_t clear_end_ns = 0;
  uint64_t check_start_ns = 0;
  uint64_t check_end_ns = 0;
  std::vector<IdlePage> pages;
};

/*
 * Requires CAP_SYS_ADMIN and CONFIG_IDLE_PAGE_TRACKING.
 * Supports only 4 KiB kernel base pages and ordinary anonymous LRU pages.
 * THP, hugetlb and other compound pages, including 2 MiB huge pages, are not
 * supported. Their addresses remain unobserved (observed == false).
 * Shared VMAs and nonresident pages also remain unobserved.
 * window_ms is the minimum wait after all idle writes complete.
 * Per-page observation intervals may be longer due to I/O ordering.
 * Throws std::exception on unsupported base page size or an incomplete scan.
 */
IdleSample sample_idle_pages(int pid, uint64_t start, uint64_t end,
                            unsigned window_ms);

#endif
