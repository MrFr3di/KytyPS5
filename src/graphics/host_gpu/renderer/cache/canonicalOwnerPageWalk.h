#ifndef KYTY_GRAPHICS_CANONICAL_OWNER_PAGE_WALK_H_
#define KYTY_GRAPHICS_CANONICAL_OWNER_PAGE_WALK_H_

// KYTY-INDEX-001 S4-B5-A: zero-extra-state canonical owner page walk.
// Both forms traverse existing MultiLevelPageTable in ascending page order;
// only never-allocated L1 buckets are skipped by the optimized form.
// Allocated-but-now-empty buckets are still scanned, by design.
// Caller must hold the same TextureCache lock for either form.
#include <cstddef>
#include <utility>

namespace Libs::Graphics {
template <bool SkipNeverAllocatedBuckets, class PageTable, class Callback>
void ForEachCanonicalOwnerPage(
    const PageTable& table,
    typename PageTable::PageRange pages,
    Callback&& callback) {
  if (pages.first >= pages.last_exclusive ||
      pages.last_exclusive > PageTable::kPageCount)
    return;
  if constexpr (SkipNeverAllocatedBuckets) {
    table.ForEachAllocatedPageInRange(
        pages,[&](size_t page,const auto& owners) {
          if (!owners.empty()) callback(page,owners);
        });
  } else {
    for (size_t page=pages.first;page<pages.last_exclusive;++page) {
      const auto* owners=table.Find(page);
      if (owners!=nullptr && !owners->empty())callback(page,*owners);
    }
  }
}
} // namespace Libs::Graphics
#endif // KYTY_GRAPHICS_CANONICAL_OWNER_PAGE_WALK_H_
