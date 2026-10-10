// KYTY-INDEX-001 S3-C2: lifetime C++ heap ledger for a complete index.
// Standalone, single-threaded, one fresh process per variant. All new/delete
// requests made while the index is constructed and queried are tagged;
// tagged allocations are decremented when freed even after the sampling ends.
// This is not an OS RSS value, nor does it count uninstrumented raw malloc.
#include "graphics/host_gpu/renderer/cache/imageRangeHybrid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <limits>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
struct alignas(std::max_align_t) AllocationHeader {
  void* base = nullptr;
  std::size_t requested = 0;
  bool tracked = false;
  bool aligned = false;
};

thread_local bool g_track = false;
thread_local uint64_t g_live = 0;
thread_local uint64_t g_peak = 0;
thread_local uint64_t g_calls = 0;
thread_local uint64_t g_frees = 0;
thread_local uint64_t g_requested_total = 0;
void TaggedNew(std::size_t n, AllocationHeader* header, void* base, bool aligned) {
  header->base = base;
  header->requested = n;
  header->tracked = g_track;
  header->aligned = aligned;
  if (header->tracked) {
    g_live += n;
    g_peak = std::max(g_peak, g_live);
    ++g_calls;
    g_requested_total += n;
  }
}
void* OrdinaryNew(std::size_t n) {
  if (n > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader))
    throw std::bad_alloc{};
  void* storage = std::malloc(sizeof(AllocationHeader) + (n ? n : 1));
  if (!storage) throw std::bad_alloc{};
  auto* header = static_cast<AllocationHeader*>(storage);
  TaggedNew(n, header, storage, false);
  return header + 1;
}
void* AlignedNew(std::size_t n, std::size_t alignment) {
#if defined(_WIN32)
  // Over-allocate to place an adjacent header immediately before the aligned
  // returned user pointer; remember the actual _aligned_malloc base for delete.
  if (alignment < alignof(AllocationHeader)) alignment = alignof(AllocationHeader);
  if (n > std::numeric_limits<std::size_t>::max() -
          sizeof(AllocationHeader) - alignment) throw std::bad_alloc{};
  void* base = _aligned_malloc((n ? n : 1) + sizeof(AllocationHeader) + alignment,
                              alignment);
  if (!base) throw std::bad_alloc{};
  const auto raw = reinterpret_cast<uintptr_t>(base) + sizeof(AllocationHeader);
  const auto user = (raw + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
  auto* header = reinterpret_cast<AllocationHeader*>(user) - 1;
  TaggedNew(n, header, base, true);
  return reinterpret_cast<void*>(user);
#else
  (void)alignment;
  return OrdinaryNew(n);
#endif
}
void TaggedDelete(void* pointer) noexcept {
  if (!pointer) return;
  auto* header = static_cast<AllocationHeader*>(pointer) - 1;
  if (header->tracked) {
    if (g_live < header->requested) std::abort();
    g_live -= header->requested;
    ++g_frees;
  }
#if defined(_WIN32)
  if (header->aligned) {
    _aligned_free(header->base);
    return;
  }
#endif
  std::free(header->base);
}
} // namespace

void* operator new(std::size_t n) { return OrdinaryNew(n); }
void* operator new[](std::size_t n) { return OrdinaryNew(n); }
void operator delete(void* p) noexcept { TaggedDelete(p); }
void operator delete[](void* p) noexcept { TaggedDelete(p); }
void operator delete(void* p, std::size_t) noexcept { TaggedDelete(p); }
void operator delete[](void* p, std::size_t) noexcept { TaggedDelete(p); }
void* operator new(std::size_t n, std::align_val_t a) {
  return AlignedNew(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return AlignedNew(n, static_cast<std::size_t>(a));
}
void operator delete(void* p, std::align_val_t) noexcept { TaggedDelete(p); }
void operator delete[](void* p, std::align_val_t) noexcept { TaggedDelete(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { TaggedDelete(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { TaggedDelete(p); }

namespace {
using Libs::Graphics::HybridImageRangeIndex;
constexpr uint64_t MiB = uint64_t{1} << 20;
constexpr uint64_t base = uint64_t{1} << 32;

int Run(int variant, uint32_t owners) {
  if (variant < 0 || variant > 2 ||
      (owners != 1 && owners != 16 && owners != 40 && owners != 256)) return 2;
  HybridImageRangeIndex::Config config;
  config.allow_snapshot = variant == 2;
  config.skip_empty_buckets = variant != 0;
  config.snapshot_after_large_queries = 1;
  config.snapshot_min_pages = 8;
  uint64_t live_after_registration = 0, live_after_snapshot = 0;
  uint64_t live_after_queries = 0, peak_when_registered = 0;
  uint64_t last_storage_estimate = 0;
  size_t actual_hits = 0, snapshots = 0;
  g_track = true;
  {
    HybridImageRangeIndex index(config);
    for (uint32_t id = 1; id <= owners; ++id) {
      if (!index.Register(id, base + static_cast<uint64_t>(id) * 8, 16 * MiB)) return 3;
    }
    live_after_registration = g_live;
    peak_when_registered = g_peak;
    if (variant == 2) {
      const auto initial = index.FindInline(base + static_cast<uint64_t>(owners) * 8,
                                            64 * MiB);
      if (initial.size() != owners || !index.HasValidSnapshot()) return 4;
    }
    live_after_snapshot = g_live;
    for (int q = 0; q < 64; ++q) {
      const auto hits = index.FindInline(base + static_cast<uint64_t>(owners) * 8,
                                         (q & 1) ? 4096 : 64 * MiB);
      if (hits.size() != owners) return 5;
      actual_hits += hits.size();
    }
    live_after_queries = g_live;
    last_storage_estimate = index.StorageEstimate();
    snapshots = index.SnapshotBuilds();
    if (!index.Validate()) return 6;
  }
  g_track = false;
  if (g_live != 0) return 7; // no tagged allocations survived index destruction
  const char* modes[] = {"legacy","H0_bucket_skip","H1_lazy_snapshot"};
  std::printf("%s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%zu,%zu,%llu\n",
              modes[variant], owners,
              static_cast<unsigned long long>(live_after_registration),
              static_cast<unsigned long long>(live_after_snapshot),
              static_cast<unsigned long long>(live_after_queries),
              static_cast<unsigned long long>(peak_when_registered),
              static_cast<unsigned long long>(g_peak),
              static_cast<unsigned long long>(g_calls),
              static_cast<unsigned long long>(g_frees),
              static_cast<unsigned long long>(g_requested_total),
              actual_hits, snapshots,
              static_cast<unsigned long long>(last_storage_estimate));
  return 0;
}
} // namespace
int main(int argc, char** argv) {
  if (argc != 3) {
    std::fputs("usage: image_range_heap_ledger <variant 0..2> <owners 1|16|40|256>\n", stderr);
    return 2;
  }
  return Run(std::atoi(argv[1]), static_cast<uint32_t>(std::atoi(argv[2])));
}
