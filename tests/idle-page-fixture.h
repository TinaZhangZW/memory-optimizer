/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IDLE_PAGE_FIXTURE_H
#define IDLE_PAGE_FIXTURE_H

#include "IdlePageMap.h"
#include <cstddef>
#include <map>
#include <vector>

namespace idle_bitmap_test {

constexpr uint64_t PAGE_SIZE_BYTES = IDLE_BITMAP_PAGE_SIZE;
constexpr unsigned TEST_PAGE_COUNT = 8;
constexpr uint64_t FIRST_PFN = 1000;
constexpr uint64_t DISTANT_PFN = 3000;
constexpr unsigned WORD_BITS = 64;
constexpr size_t WORD_BYTES = sizeof(uint64_t);
constexpr uint64_t PRESENT_BIT = 1ULL << 63;
constexpr uint64_t SWAPPED_BIT = 1ULL << 62;

struct FixtureState
{
  uint64_t range_start = 0;
  size_t page_count = TEST_PAGE_COUNT;
  bool use_default_page_states = true;
  /*
   * Optional per-page PFNs; NO_PFN produces a nonresident pagemap entry.
   * An empty vector uses the default mapping and alias/gap controls.
   */
  std::vector<uint64_t> page_pfns;
  unsigned pagemap_reads = 0;
  unsigned pagemap_passes = 0;
  unsigned flags_reads = 0;
  // Requested entries, including entries read through physical address gaps.
  size_t flag_words = 0;
  bool short_read = false;
  bool io_error = false;
  bool sparse_pfns = false;
  unsigned bitmap_writes = 0;
  unsigned bitmap_reads = 0;
  /*
   * Counters include failed calls. Zero disables each short-transfer trigger.
   * A triggered transfer completes all but its final 64-bit entry.
   */
  unsigned short_pagemap_read_call = 0;
  unsigned short_flags_read_call = 0;
  unsigned short_bitmap_read_call = 0;
  unsigned short_bitmap_write_call = 0;
  bool lock_busy = false;
  bool lose_lru = false;
  bool short_write = false;
  std::map<uint64_t, uint64_t> written_bitmap;
  bool alias_last_page = false;
  bool adjacent_words = false;
};

extern FixtureState state;

// A real anonymous VMA with synthetic PFNs and kernel metadata.
class PageFixture
{
public:
  explicit PageFixture(size_t page_count = TEST_PAGE_COUNT);
  ~PageFixture();
  PageFixture(const PageFixture&) = delete;
  PageFixture& operator=(const PageFixture&) = delete;

  uint64_t start() const
  {
    return state.range_start;
  }
  uint64_t end() const
  {
    return start() + page_count_ * PAGE_SIZE_BYTES;
  }
  void reset();

private:
  size_t page_count_;
  void* memory_;
};

} // namespace idle_bitmap_test

#endif
