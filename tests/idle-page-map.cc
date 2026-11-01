/* SPDX-License-Identifier: GPL-2.0 */
#include "IdlePageMap.h"
#include "IdlePageIO.h"
#include "idle-page-fixture.h"

#include <cassert>
#include <fcntl.h>

namespace {
using namespace idle_bitmap_test;

std::vector<uint64_t> read_fixture(const PageFixture& fixture)
{
  const std::string path = "/proc/" + std::to_string(getpid()) + "/pagemap";
  idle_bitmap::FileDescriptor pagemap(open(path.c_str(), O_RDONLY),
                                     "open test pagemap");
  idle_bitmap::FileDescriptor flags(open("/proc/kpageflags", O_RDONLY),
                                   "open test flags");
  return idle_bitmap::read_eligible_pfns(
      getpid(), pagemap.get(), flags.get(), fixture.start(), fixture.end());
}

void test_eligibility_and_aliases()
{
  PageFixture fixture;
  const auto pfns = read_fixture(fixture);
  assert(pfns.size() == TEST_PAGE_COUNT);
  assert(state.pagemap_reads == 1);
  assert(state.flags_reads == 2);
  assert(state.flag_words == TEST_PAGE_COUNT - 1);
  for (unsigned i = 0; i < TEST_PAGE_COUNT; ++i) {
    const uint64_t expected = (i == 2 || i == 3)
        ? idle_bitmap::NO_PFN : FIRST_PFN + i;
    assert(pfns[i] == expected);
  }

  fixture.reset();
  state.sparse_pfns = true;
  assert(read_fixture(fixture).back() == DISTANT_PFN);
  assert(state.flags_reads == 3);
  assert(state.flag_words == TEST_PAGE_COUNT - 1);

  fixture.reset();
  state.alias_last_page = true;
  assert(read_fixture(fixture).back() == FIRST_PFN);
  assert(state.flags_reads == 2);
  assert(state.flag_words == TEST_PAGE_COUNT - 2);

  for (bool short_transfer : {false, true}) {
    fixture.reset();
    state.short_read = short_transfer;
    state.io_error = !short_transfer;
    bool failed = false;
    try {
      read_fixture(fixture);
    } catch (const std::exception&) {
      failed = true;
    }
    assert(failed);
  }
}

void test_batch_boundaries()
{
  constexpr size_t BATCH_WORDS = PAGE_SIZE_BYTES / WORD_BYTES;
  for (size_t pages : {BATCH_WORDS - 1, BATCH_WORDS, BATCH_WORDS + 1}) {
    PageFixture fixture(pages);
    state.use_default_page_states = false;

    const auto pfns = read_fixture(fixture);
    assert(pfns.size() == pages);
    for (size_t i = 0; i < pages; ++i)
      assert(pfns[i] == FIRST_PFN + i);

    const size_t batches = (pages + BATCH_WORDS - 1) / BATCH_WORDS;
    assert(state.pagemap_reads == batches);
    assert(state.flags_reads == batches);
    assert(state.flag_words == pages);
  }
}

void test_unsorted_pfns()
{
  PageFixture fixture(4);
  state.page_pfns = {FIRST_PFN + 3, FIRST_PFN + 7, FIRST_PFN, FIRST_PFN + 1};

  // Physical flags belong to the PFN, not its position in HVA order.
  const std::vector<uint64_t> expected = {
    idle_bitmap::NO_PFN, FIRST_PFN + 7, idle_bitmap::NO_PFN, FIRST_PFN + 1
  };
  assert(read_fixture(fixture) == expected);
}

void test_sparse_read_amplification()
{
  struct ReadCase
  {
    std::vector<uint64_t> pfns;
    unsigned calls;
    size_t words;
  };
  const std::vector<ReadCase> cases = {
    {{FIRST_PFN, FIRST_PFN + 1}, 1, 2},
    {{FIRST_PFN, FIRST_PFN, FIRST_PFN + 1}, 1, 2},
    {{FIRST_PFN, FIRST_PFN + 2}, 2, 2},
    {{FIRST_PFN, FIRST_PFN + 3}, 2, 2},
    {{FIRST_PFN, FIRST_PFN + 4}, 2, 2},
    {{FIRST_PFN, FIRST_PFN + 511}, 2, 2},
    {{FIRST_PFN, FIRST_PFN, FIRST_PFN + 4}, 2, 2},
    {{FIRST_PFN, FIRST_PFN + 1, FIRST_PFN + 2, FIRST_PFN + 3,
      FIRST_PFN + 4, FIRST_PFN + 5, FIRST_PFN + 6, FIRST_PFN + 7,
      FIRST_PFN + 511}, 2, 9},
    {{FIRST_PFN, idle_bitmap::NO_PFN, FIRST_PFN + 1}, 1, 2},
    {{idle_bitmap::NO_PFN, idle_bitmap::NO_PFN}, 0, 0}
  };

  for (const auto& example : cases) {
    PageFixture fixture(example.pfns.size());
    state.use_default_page_states = false;
    state.page_pfns = example.pfns;

    assert(read_fixture(fixture) == example.pfns);
    assert(state.flags_reads == example.calls);
    assert(state.flag_words == example.words);
  }
}

void test_later_batch_failures()
{
  constexpr size_t BATCH_WORDS = PAGE_SIZE_BYTES / WORD_BYTES;
  for (bool fail_pagemap : {true, false}) {
    // Return one entry of the second batch before a short-transfer failure.
    PageFixture fixture(BATCH_WORDS + 2);
    state.use_default_page_states = false;
    if (fail_pagemap)
      state.short_pagemap_read_call = 2;
    else
      state.short_flags_read_call = 2;

    bool failed = false;
    try {
      read_fixture(fixture);
    } catch (const std::exception&) {
      failed = true;
    }
    assert(failed);
    assert(state.pagemap_reads == 2);
    assert(state.flags_reads == (fail_pagemap ? 0U : 2U));
  }
}
} // namespace

int main()
{
  test_eligibility_and_aliases();
  test_batch_boundaries();
  test_unsorted_pfns();
  test_sparse_read_amplification();
  test_later_batch_failures();
}
