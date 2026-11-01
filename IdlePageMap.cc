/* SPDX-License-Identifier: GPL-2.0 */

#include "IdlePageMap.h"
#include "IdlePageIO.h"

#include <algorithm>
#include <fstream>
#include <linux/kernel-page-flags.h>
#include <sstream>

namespace idle_bitmap {
namespace {

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

    for (uint64_t address = range_start; address < range_end;
         address += IDLE_BITMAP_PAGE_SIZE) {
      uint64_t entry;

      // Pagemap is indexed by virtual page number, not by VMA offset.
      const uint64_t offset = address / IDLE_BITMAP_PAGE_SIZE * WORD_BYTES;
      transfer_words(pagemap_fd, IOOperation::READ, offset, &entry, 1);

      const uint64_t pfn = entry & PAGEMAP_PFN_MASK;
      if ((entry & PAGEMAP_PRESENT) != 0 && pfn == 0)
        throw std::runtime_error(
            "present page has PFN 0: check CAP_SYS_ADMIN/PFN access");

      // Result indices are relative to the requested range, not this VMA.
      if ((entry & PAGEMAP_PRESENT) != 0 &&
          (entry & PAGEMAP_SWAPPED) == 0)
        pfns[(address - start) / IDLE_BITMAP_PAGE_SIZE] = pfn;
    }
  }

  if (maps.bad())
    throw std::runtime_error("failed reading " + maps_path);

  return pfns;
}

} // namespace

std::vector<uint64_t> read_eligible_pfns(
    int pid, int pagemap_fd, int flags_fd, uint64_t start, uint64_t end)
{
  auto pfns = read_present_pfns(pid, pagemap_fd, start, end);

  for (uint64_t& pfn : pfns) {
    if (pfn == NO_PFN)
      continue;

    uint64_t flags;
    transfer_words(flags_fd, IOOperation::READ, pfn * WORD_BYTES, &flags, 1);
    const bool has_required_flags =
        (flags & REQUIRED_PAGE_FLAGS) == REQUIRED_PAGE_FLAGS;
    const bool has_excluded_flags = (flags & EXCLUDED_PAGE_FLAGS) != 0;

    if (!has_required_flags || has_excluded_flags)
      pfn = NO_PFN;
  }

  return pfns;
}

} // namespace idle_bitmap
