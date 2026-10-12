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
// S4-B5-B: stateless dispatch, using only a validated half-open PageRange.
// A crossing of a 1-GiB L1 boundary is necessary, not sufficient, to skip.
// Subtraction and last_exclusive - 1 occur only after range validation.
template <size_t MinimumPages, class PageTable>
[[nodiscard]] constexpr bool ShouldSkipCanonicalBuckets(
    typename PageTable::PageRange pages) noexcept {
  static_assert(MinimumPages > 0);
  if (pages.first >= pages.last_exclusive ||
      pages.last_exclusive > PageTable::kPageCount)
    return false;
  return (pages.last_exclusive - pages.first >= MinimumPages) &&
         ((pages.first / PageTable::kBucketEntries) !=
          ((pages.last_exclusive - 1) / PageTable::kBucketEntries));
}

template <size_t MinimumPages, class PageTable, class Callback>
bool ForEachAdaptiveCanonicalOwnerPage(
    const PageTable& table,
    typename PageTable::PageRange pages,
    Callback&& callback) {
  const bool skip = ShouldSkipCanonicalBuckets<MinimumPages, PageTable>(pages);
  if (skip) {
    ForEachCanonicalOwnerPage<true>(table, pages, std::forward<Callback>(callback));
  } else {
    ForEachCanonicalOwnerPage<false>(table, pages, std::forward<Callback>(callback));
  }
  return skip;
}

// S4-B5-C: validated page-count-only switch, even within one L1 bucket.
// The canonical table and callback are identical for either path.
template <size_t MinimumPages, class PageTable>
[[nodiscard]] constexpr bool ShouldSkipCanonicalBucketsByPageCount(
    typename PageTable::PageRange pages) noexcept {
  static_assert(MinimumPages > 0);
  return pages.first < pages.last_exclusive &&
         pages.last_exclusive <= PageTable::kPageCount &&
         pages.last_exclusive - pages.first >= MinimumPages;
}

template <size_t MinimumPages, class PageTable, class Callback>
bool ForEachPageCountCanonicalOwnerPage(
    const PageTable& table,
    typename PageTable::PageRange pages,
    Callback&& callback) {
  const bool skip = ShouldSkipCanonicalBucketsByPageCount<MinimumPages, PageTable>(pages);
  if (skip) {
    ForEachCanonicalOwnerPage<true>(table, pages, std::forward<Callback>(callback));
  } else {
    ForEachCanonicalOwnerPage<false>(table, pages, std::forward<Callback>(callback));
  }
  return skip;
}
} // namespace Libs::Graphics
#endif // KYTY_GRAPHICS_CANONICAL_OWNER_PAGE_WALK_H_
