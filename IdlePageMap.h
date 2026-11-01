/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IDLE_PAGE_MAP_H
#define IDLE_PAGE_MAP_H

#include <cstdint>
#include <limits>
#include <vector>

// The idle bitmap backend supports 4 KiB base pages.
constexpr uint64_t IDLE_BITMAP_PAGE_SIZE = 4096;

namespace idle_bitmap {

constexpr uint64_t NO_PFN = std::numeric_limits<uint64_t>::max();

/*
 * Return one eligible PFN or NO_PFN per base page in [start, end).
 * Eligible pages are ordinary anonymous LRU pages.
 *
 * Bounds must be ordered and aligned to IDLE_BITMAP_PAGE_SIZE.
 * pagemap_fd must refer to /proc/PID/pagemap; flags_fd to /proc/kpageflags.
 * The caller owns both descriptors and handles target exit or PID reuse.
 */
std::vector<uint64_t> read_eligible_pfns(
    int pid, int pagemap_fd, int flags_fd, uint64_t start, uint64_t end);

} // namespace idle_bitmap

#endif
