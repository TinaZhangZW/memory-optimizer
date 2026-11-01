/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IDLE_PAGE_FIXTURE_H
#define IDLE_PAGE_FIXTURE_H

#include "IdlePageMap.h"
#include <cstddef>
#include <map>

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
  unsigned pagemap_reads = 0;
  unsigned pagemap_passes = 0;
  unsigned flags_reads = 0;
  size_t flag_words = 0;
  bool short_read = false;
  bool io_error = false;
  bool sparse_pfns = false;
};

extern FixtureState state;

// A real anonymous VMA with synthetic PFNs and kernel metadata.
class PageFixture
{
public:
  PageFixture();
  ~PageFixture();
  PageFixture(const PageFixture&) = delete;
  PageFixture& operator=(const PageFixture&) = delete;

  uint64_t start() const
  {
    return state.range_start;
  }
  uint64_t end() const
  {
    return start() + TEST_PAGE_COUNT * PAGE_SIZE_BYTES;
  }
  void reset();

private:
  void* memory_;
};

} // namespace idle_bitmap_test

#endif
