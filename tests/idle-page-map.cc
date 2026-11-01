/* SPDX-License-Identifier: GPL-2.0 */
#include "IdlePageMap.h"
#include "IdlePageIO.h"
#include "idle-page-fixture.h"

#include <cassert>
#include <fcntl.h>

int main()
{
  using namespace idle_bitmap_test;
  using idle_bitmap::FileDescriptor;
  PageFixture fixture;

  const auto read = [&fixture]() {
    const std::string path = "/proc/" + std::to_string(getpid()) + "/pagemap";
    FileDescriptor pagemap(open(path.c_str(), O_RDONLY), "open test pagemap");
    FileDescriptor flags(open("/proc/kpageflags", O_RDONLY), "open test flags");
    return idle_bitmap::read_eligible_pfns(
        getpid(), pagemap.get(), flags.get(), fixture.start(), fixture.end());
  };

  const auto pfns = read();
  assert(pfns.size() == TEST_PAGE_COUNT);
  for (unsigned i = 0; i < TEST_PAGE_COUNT; ++i) {
    const uint64_t expected = (i == 2 || i == 3)
        ? idle_bitmap::NO_PFN : FIRST_PFN + i;
    assert(pfns[i] == expected);
  }

  fixture.reset();
  state.sparse_pfns = true;
  assert(read().back() == DISTANT_PFN);

  for (bool short_transfer : {false, true}) {
    fixture.reset();
    state.short_read = short_transfer;
    state.io_error = !short_transfer;
    bool failed = false;
    try {
      read();
    } catch (const std::exception&) {
      failed = true;
    }
    assert(failed);
  }
}
