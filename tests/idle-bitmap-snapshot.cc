/* SPDX-License-Identifier: GPL-2.0 */
#include "IdleBitmapScan.h"
#include "idle-page-fixture.h"

#include <cassert>
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

} // namespace

int main()
{
  using namespace idle_bitmap_test;
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
