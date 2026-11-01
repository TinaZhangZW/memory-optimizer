/* SPDX-License-Identifier: GPL-2.0 */
#include "IdleBitmapScan.h"
#include "idle-page-fixture.h"

#include <cassert>
#include <iterator>
#include <stdexcept>
#include <unistd.h>

namespace {
enum class Failure {
  SHORT_READ,
  IO_ERROR,
  INVALID_WINDOW,
  BUSY_LOCK,
  SHORT_WRITE
};

using namespace idle_bitmap_test;

void test_basic_sampling()
{
  PageFixture fixture;
  const uint64_t range_start = fixture.start();
  const uint64_t range_end = fixture.end();
  auto sample = sample_idle_pages(getpid(), range_start, range_end, 1);

  assert(sample.pages.size() == TEST_PAGE_COUNT && state.bitmap_writes == 1);
  assert(sample.pages[0].observed && sample.pages[0].refs == 0);
  assert(sample.pages[1].observed && sample.pages[1].refs == 1);

  for (unsigned i = 2; i <= 5; ++i)
    assert(!sample.pages[i].observed);

  assert(sample.pages[6].observed && sample.pages[7].observed);

  uint64_t expected = 0;

  for (unsigned i : {0, 1, 4, 5, 6, 7})
    expected |= 1ULL << ((FIRST_PFN + i) % WORD_BITS);

  assert(state.written_bitmap.size() == 1);
  assert(state.written_bitmap.at(FIRST_PFN / WORD_BITS) == expected);
  assert(sample.clear_start_ns <= sample.clear_end_ns);
  assert(sample.check_start_ns - sample.clear_end_ns >= 1000000);
  assert(sample.check_start_ns <= sample.check_end_ns);

  // A large PFN gap must produce separate bitmap writes, not a wide mask.
  fixture.reset();
  state.sparse_pfns = true;
  sample = sample_idle_pages(getpid(), range_start, range_end, 1);
  assert(state.bitmap_writes == 2);
  assert(state.written_bitmap.size() == 2);
  assert(state.written_bitmap.at(DISTANT_PFN / WORD_BITS) ==
         (1ULL << (DISTANT_PFN % WORD_BITS)));
  assert(sample.pages.back().observed);
  const uint64_t fixed_bits =
      expected & ~(1ULL << ((FIRST_PFN + 7) % WORD_BITS));
  assert(state.written_bitmap.at(FIRST_PFN / WORD_BITS) == fixed_bits);

  // Adjacent bitmap words can share a syscall without widening write masks.
  fixture.reset();
  state.adjacent_words = true;
  sample = sample_idle_pages(getpid(), range_start, range_end, 1);
  assert(state.bitmap_writes == 1);
  assert(state.written_bitmap.size() == 2);
  const uint64_t adjacent_pfn = FIRST_PFN + WORD_BITS;
  assert(state.written_bitmap.at(adjacent_pfn / WORD_BITS) ==
         (1ULL << (adjacent_pfn % WORD_BITS)));
  assert(state.written_bitmap.at(FIRST_PFN / WORD_BITS) == fixed_bits);

  // A stable PFN is not enough if it leaves the eligible LRU set.
  fixture.reset();
  state.lose_lru = true;
  sample = sample_idle_pages(getpid(), range_start, range_end, 1);
  assert(!sample.pages[6].observed);

  for (const Failure failure : {Failure::SHORT_READ, Failure::IO_ERROR,
                          Failure::INVALID_WINDOW, Failure::BUSY_LOCK,
                          Failure::SHORT_WRITE}) {
    fixture.reset();
    state.short_read = failure == Failure::SHORT_READ;
    state.io_error = failure == Failure::IO_ERROR;
    state.lock_busy = failure == Failure::BUSY_LOCK;
    state.short_write = failure == Failure::SHORT_WRITE;
    const unsigned window_ms = failure == Failure::INVALID_WINDOW ? 0 : 1;
    bool failed = false;

    try {
      sample_idle_pages(getpid(), range_start, range_end, window_ms);
    } catch (const std::exception&) {
      failed = true;
    }

    assert(failed);
  }
}

std::map<uint64_t, uint64_t> configure_bitmap_words(size_t pages)
{
  state.use_default_page_states = false;
  std::map<uint64_t, uint64_t> masks;
  for (size_t i = 0; i < pages; ++i) {
    const uint64_t pfn = FIRST_PFN + i * WORD_BITS;
    state.page_pfns.push_back(pfn);
    masks[pfn / WORD_BITS] |= 1ULL << (pfn % WORD_BITS);
  }
  return masks;
}

void test_bitmap_batch_boundaries()
{
  constexpr size_t BATCH_WORDS = PAGE_SIZE_BYTES / WORD_BYTES;
  for (size_t pages : {BATCH_WORDS - 1, BATCH_WORDS, BATCH_WORDS + 1}) {
    PageFixture fixture(pages);
    const auto masks = configure_bitmap_words(pages);
    const auto sample = sample_idle_pages(
        getpid(), fixture.start(), fixture.end(), 1);

    assert(sample.pages.size() == pages);
    for (size_t i = 0; i < pages; ++i) {
      assert(sample.pages[i].hva == fixture.start() + i * PAGE_SIZE_BYTES);
      assert(sample.pages[i].observed && sample.pages[i].refs == 0);
    }

    const size_t batches = (pages + BATCH_WORDS - 1) / BATCH_WORDS;
    assert(state.bitmap_writes == batches);
    assert(state.bitmap_reads == batches);
    assert(state.written_bitmap == masks);
  }
}

void test_later_bitmap_failures()
{
  constexpr size_t BATCH_WORDS = PAGE_SIZE_BYTES / WORD_BYTES;
  for (bool fail_write : {true, false}) {
    // Return one word of the second batch before reporting a short transfer.
    PageFixture fixture(BATCH_WORDS + 2);
    auto masks = configure_bitmap_words(BATCH_WORDS + 2);
    if (fail_write)
      state.short_bitmap_write_call = 2;
    else
      state.short_bitmap_read_call = 2;

    bool failed = false;
    try {
      sample_idle_pages(getpid(), fixture.start(), fixture.end(), 1);
    } catch (const std::exception&) {
      failed = true;
    }
    assert(failed);
    assert(state.bitmap_writes == 2);
    assert(state.bitmap_reads == (fail_write ? 0U : 2U));
    assert(state.pagemap_passes == 1);

    // Completed writes are not rolled back when the scan fails.
    if (fail_write)
      masks.erase(std::prev(masks.end()));
    assert(state.written_bitmap == masks);
  }
}
} // namespace

int main()
{
  test_basic_sampling();
  test_bitmap_batch_boundaries();
  test_later_bitmap_failures();
}
