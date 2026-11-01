/* SPDX-License-Identifier: GPL-2.0 */

#include "IdlePageMap.h"
#include "IdlePageIO.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <linux/kernel-page-flags.h>
#include <sstream>

namespace idle_bitmap {
namespace {

constexpr size_t IO_BATCH_WORDS = IDLE_BITMAP_PAGE_SIZE / WORD_BYTES;

struct PageFlags
{
  uint64_t pfn;
  uint64_t flags;
};

using PageMetadata = std::vector<PageFlags>;

// /proc/PID/pagemap: bits 0..54 encode the PFN only for present pages.
constexpr uint64_t PAGEMAP_PRESENT = 1ULL << 63;
constexpr uint64_t PAGEMAP_SWAPPED = 1ULL << 62;
constexpr uint64_t PAGEMAP_PFN_MASK = (1ULL << 55) - 1;

constexpr uint64_t REQUIRED_PAGE_FLAGS = (1ULL << KPF_ANON) | (1ULL << KPF_LRU);
constexpr uint64_t EXCLUDED_PAGE_FLAGS =
    (1ULL << KPF_THP) | (1ULL << KPF_HUGE) |
    (1ULL << KPF_COMPOUND_HEAD) | (1ULL << KPF_COMPOUND_TAIL) |
    (1ULL << KPF_KSM) | (1ULL << KPF_UNEVICTABLE) |
    (1ULL << KPF_BUDDY) | (1ULL << KPF_HWPOISON) | (1ULL << KPF_NOPAGE);

/*
 * Resolve resident PFNs in readable, writable private VMAs with no
 * file inode. Preserve one slot per base page in [start, end).
 * Physical-page eligibility is checked by read_eligible_pfns().
 */
std::vector<uint64_t> read_present_pfns(int pid, int pagemap_fd,
                                      uint64_t start, uint64_t end)
{
  std::vector<uint64_t> pfns((end - start) / IDLE_BITMAP_PAGE_SIZE, NO_PFN);
  const std::string maps_path = "/proc/" + std::to_string(pid) + "/maps";
  std::ifstream maps(maps_path);

  if (!maps)
    throw std::runtime_error("cannot read " + maps_path);

  std::string line;
  while (std::getline(maps, line)) {
    uint64_t vma_start;
    uint64_t vma_end;
    uint64_t file_offset;
    uint64_t inode;
    char separator;
    std::string permissions;
    std::string device;
    std::istringstream fields(line);

    if (!(fields >> std::hex >> vma_start >> separator >> vma_end >>
          permissions >> file_offset >> device >> std::dec >> inode) ||
        separator != '-' || permissions.size() != 4)
      throw std::runtime_error("invalid target maps entry");

    if (permissions[0] != 'r' || permissions[1] != 'w' ||
        permissions[3] != 'p' || inode != 0)
      continue;

    const uint64_t range_start = std::max(vma_start, start);
    const uint64_t range_end = std::min(vma_end, end);

    for (uint64_t address = range_start; address < range_end;) {
      std::array<uint64_t, IO_BATCH_WORDS> entries;
      const size_t count = std::min<uint64_t>(
          entries.size(), (range_end - address) / IDLE_BITMAP_PAGE_SIZE);

      // Pagemap is indexed by virtual page number, not by VMA offset.
      const uint64_t offset = address / IDLE_BITMAP_PAGE_SIZE * WORD_BYTES;
      transfer_words(pagemap_fd, IOOperation::READ, offset,
                     entries.data(), count);

      for (size_t i = 0; i < count; ++i) {
        const uint64_t entry = entries[i];
        const uint64_t pfn = entry & PAGEMAP_PFN_MASK;

        if ((entry & PAGEMAP_PRESENT) != 0 && pfn == 0)
          throw std::runtime_error(
              "present page has PFN 0: check CAP_SYS_ADMIN/PFN access");

        if ((entry & PAGEMAP_PRESENT) != 0 &&
            (entry & PAGEMAP_SWAPPED) == 0) {
          // Result indices are relative to the requested range, not this VMA.
          const size_t page_index = (address - start) / IDLE_BITMAP_PAGE_SIZE;
          pfns[page_index + i] = pfn;
        }
      }

      address += count * IDLE_BITMAP_PAGE_SIZE;
    }
  }

  if (maps.bad())
    throw std::runtime_error("failed reading " + maps_path);

  return pfns;
}

PageMetadata read_page_flags(int flags_fd, const std::vector<uint64_t>& pfns)
{
  /*
   * Store metadata contiguously rather than allocating a tree node per PFN.
   * Sorting groups physical runs; unique removes aliases before flag reads.
   */
  PageMetadata metadata;
  const auto candidates = std::count_if(
      pfns.begin(), pfns.end(), [](uint64_t pfn) { return pfn != NO_PFN; });
  metadata.reserve(static_cast<size_t>(candidates));
  for (const uint64_t pfn : pfns) {
    if (pfn != NO_PFN)
      metadata.push_back({pfn, 0});
  }

  std::sort(metadata.begin(), metadata.end(),
            [](const PageFlags& left, const PageFlags& right) {
              return left.pfn < right.pfn;
            });
  const auto unique_end = std::unique(
      metadata.begin(), metadata.end(),
      [](const PageFlags& left, const PageFlags& right) {
        return left.pfn == right.pfn;
      });
  metadata.erase(unique_end, metadata.end());

  for (auto batch_begin = metadata.begin(); batch_begin != metadata.end();) {
    const uint64_t first_pfn = batch_begin->pfn;
    auto batch_end = batch_begin;
    size_t count = 0;

    /*
     * Merge only consecutive PFNs. Reading through a hole adds kernel work
     * for pages that were not requested, even when the transfer fits in 4 KiB.
     */
    while (batch_end != metadata.end() &&
           count < IO_BATCH_WORDS && batch_end->pfn == first_pfn + count) {
      ++count;
      ++batch_end;
    }

    std::array<uint64_t, IO_BATCH_WORDS> flags;

    transfer_words(flags_fd, IOOperation::READ, first_pfn * WORD_BYTES,
                   flags.data(), count);

    while (batch_begin != batch_end) {
      batch_begin->flags = flags[batch_begin->pfn - first_pfn];
      ++batch_begin;
    }
  }

  return metadata;
}

uint64_t find_page_flags(const PageMetadata& metadata, uint64_t pfn)
{
  const auto entry = std::lower_bound(
      metadata.begin(), metadata.end(), pfn,
      [](const PageFlags& candidate, uint64_t value) {
        return candidate.pfn < value;
      });

  if (entry == metadata.end() || entry->pfn != pfn)
    throw std::logic_error("missing metadata for a present PFN");

  return entry->flags;
}

} // namespace

std::vector<uint64_t> read_eligible_pfns(
    int pid, int pagemap_fd, int flags_fd, uint64_t start, uint64_t end)
{
  auto pfns = read_present_pfns(pid, pagemap_fd, start, end);
  const auto metadata = read_page_flags(flags_fd, pfns);

  for (uint64_t& pfn : pfns) {
    if (pfn == NO_PFN)
      continue;

    const uint64_t flags = find_page_flags(metadata, pfn);
    const bool has_required_flags =
        (flags & REQUIRED_PAGE_FLAGS) == REQUIRED_PAGE_FLAGS;
    const bool has_excluded_flags = (flags & EXCLUDED_PAGE_FLAGS) != 0;

    if (!has_required_flags || has_excluded_flags)
      pfn = NO_PFN;
  }

  return pfns;
}

} // namespace idle_bitmap
