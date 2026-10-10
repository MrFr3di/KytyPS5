// KYTY-INDEX-001 H0/H1: independent page-owner oracle vs hybrid.
#include "graphics/host_gpu/renderer/cache/imageRangeHybrid.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using Libs::Graphics::HybridImageRangeIndex;
constexpr uint64_t kLimit = uint64_t{1} << 44;
constexpr uint64_t kMiB = uint64_t{1} << 20;
constexpr uint64_t kFourK = uint64_t{1} << 12;
void Check(bool success, const char* message) {
  if (!success) { std::fprintf(stderr, "ImageRangeHybridTests: %s\n", message); std::abort(); }
}

struct LegacyOracle {
  struct Info {
    uint64_t registered_begin, registered_end;
    uint64_t effective_begin, effective_end;
  };
  std::unordered_map<uint32_t, Info> live;
  std::map<uint32_t, std::vector<uint32_t>> owners;

  bool Register(uint32_t id, uint64_t address, uint64_t size) {
    if (!size || address >= kLimit || size > kLimit - address || live.contains(id)) {
      return false;
    }
    live.emplace(id, Info{address, address + size, address, address + size});
    for (uint64_t page = address / kMiB; page <= (address + size - 1) / kMiB; ++page) {
      owners[static_cast<uint32_t>(page)].push_back(id);
    }
    return true;
  }
  bool Unregister(uint32_t id) {
    auto found = live.find(id);
    if (found == live.end()) return false;
    const auto info = found->second;
    for (uint64_t page = info.registered_begin / kMiB;
         page <= (info.registered_end - 1) / kMiB; ++page) {
      auto& bucket = owners.at(static_cast<uint32_t>(page));
      auto it = std::find(bucket.begin(), bucket.end(), id);
      Check(it != bucket.end(), "oracle missing registered owner");
      bucket.erase(it);
      if (bucket.empty()) owners.erase(static_cast<uint32_t>(page));
    }
    live.erase(found);
    return true;
  }
  bool UpdateEffective(uint32_t id, uint64_t address, uint64_t size) {
    if (!size || address >= kLimit || size > kLimit - address) return false;
    auto found = live.find(id);
    if (found == live.end()) return false;
    found->second.effective_begin = address;
    found->second.effective_end = address + size;
    return true;
  }
  std::vector<uint32_t> Find(uint64_t address, uint64_t size, bool page_overlap) const {
    if (!size || address >= kLimit || size > kLimit - address) return {};
    std::vector<uint32_t> result;
    std::unordered_set<uint32_t> seen;
    const auto end = address + size;
    for (uint64_t page = address / kMiB; page <= (end - 1) / kMiB; ++page) {
      const auto it = owners.find(static_cast<uint32_t>(page));
      if (it == owners.end()) continue;
      for (const auto id : it->second) {
        if (!seen.insert(id).second) continue;
        const auto& image = live.at(id);
        bool match = image.effective_begin < end && address < image.effective_end;
        if (page_overlap) {
          match = (image.effective_begin / kFourK <= (end - 1) / kFourK) &&
                  (address / kFourK <= (image.effective_end - 1) / kFourK);
        }
        if (match) result.push_back(id);
      }
    }
    return result;
  }
};



struct Fixture {
  LegacyOracle oracle;
  HybridImageRangeIndex hybrid;
  explicit Fixture(HybridImageRangeIndex::Config config) : hybrid(config) {}
  void Register(uint32_t id, uint64_t address, uint64_t size) {
    const bool ok = oracle.Register(id, address, size);
    Check(hybrid.Register(id, address, size) == ok, "register mismatch");
    Check(hybrid.Validate(), "snapshot invariant after register");
  }
  void Unregister(uint32_t id) {
    const bool ok = oracle.Unregister(id);
    Check(hybrid.Unregister(id) == ok, "unregister mismatch");
    Check(hybrid.Validate(), "snapshot invariant after unregister");
  }
  void Update(uint32_t id, uint64_t address, uint64_t size) {
    const bool ok = oracle.UpdateEffective(id, address, size);
    Check(hybrid.UpdateEffective(id, address, size) == ok, "mutable range mismatch");
    Check(hybrid.Validate(), "snapshot invariant after effective-range mutation");
  }
  HybridImageRangeIndex::QueryStats Query(uint64_t address, uint64_t size) {
    HybridImageRangeIndex::QueryStats stats;
    HybridImageRangeIndex::QueryStats first_stats;
    for (bool pages : {false,true}) {
      const auto expected = oracle.Find(address, size, pages);
      const auto actual = hybrid.Find(address, size, pages, &stats);
      if (!pages) first_stats = stats;
      if (actual != expected) {
        std::fprintf(stderr, "Hybrid mismatch address=%llx size=%llx pages=%d"
                             " expected=%zu actual=%zu generation=%llu snapshot=%zu\n",
                     static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(size), int(pages),
                     expected.size(), actual.size(),
                     static_cast<unsigned long long>(hybrid.Generation()),
                     hybrid.SnapshotBuilds());
        std::abort();
      }
    }
    return first_stats;
  }
};

uint64_t Next(uint64_t& s) {
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}

void TestH0BucketSkipping() {
  HybridImageRangeIndex::Config config;
  config.snapshot_min_pages = 4;
  config.allow_snapshot = false;
  Fixture fixture(config);
  constexpr uint64_t GiB = 1024 * kMiB;
  fixture.Register(1, 14 * kMiB + 4095, 2);
  fixture.Register(2, 7 * GiB - 1, 3);
  fixture.Register(3, 15 * GiB + 125, kFourK);
  auto scan = fixture.Query(1, 16 * GiB);
  Check(scan.path == HybridImageRangeIndex::QueryPath::BucketSkip,
        "long query must use allocated buckets");
  // A cross-boundary image occupies two buckets; four allocated 1-GiB buckets total.
  Check(scan.visited_pages == 4 * 1024, "H0 skips 12 completely unallocated buckets");
  fixture.Query(14 * kMiB, 4096);
  fixture.Query(7 * GiB - 1, 2);
  fixture.Query(15 * GiB + 126, 1);
  fixture.Unregister(1);
  fixture.Unregister(2);
  fixture.Unregister(3);
  // Unregistered buckets remain allocated in the canonical page table.
  fixture.Query(1, 16 * GiB);
  Check(fixture.hybrid.SnapshotBuilds() == 0, "H0 must not build snapshots");
}

void TestH1LazyLifecycle() {
  HybridImageRangeIndex::Config config;
  config.snapshot_min_pages = 4;
  config.snapshot_after_large_queries = 5;
  config.block_size = 7;
  Fixture fixture(config);
  constexpr uint64_t base = uint64_t{1} << 32;
  fixture.Register(1, base + 12, 23 * kMiB);
  fixture.Register(2, base + 2 * kMiB, 1024);
  fixture.Register(3, base + 3 * kMiB, 15 * kMiB);
  fixture.Register(4, base + 4 * kMiB, 4 * kMiB);
  Check(!fixture.hybrid.HasValidSnapshot(), "no eager build on registration");
  for (int i = 0; i < 2; ++i) {
    const auto stats = fixture.Query(base, 32 * kMiB);
    Check(stats.path == HybridImageRangeIndex::QueryPath::BucketSkip,
          "large query uses H0 until amortization gate");
  }
  const auto ready = fixture.Query(base, 32 * kMiB);
  Check(ready.path == HybridImageRangeIndex::QueryPath::Snapshot,
        "fifth qualifying lookup builds during third paired query");
  Check(ready.snapshot_build_records == 4, "snapshot includes all live images");
  Check(fixture.hybrid.SnapshotBuilds() == 1 && fixture.hybrid.HasValidSnapshot(),
        "snapshot is valid after lazy build");
  fixture.Query(base, 32 * kMiB);
  Check(fixture.hybrid.SnapshotBuilds() == 1, "reuse current snapshot without rebuild");
  fixture.Query(base + 2 * kMiB, 16); // small fast path, even with snapshot
  Check(fixture.hybrid.SnapshotBuilds() == 1, "small lookup never triggers build");

  // effective image range may move without re-registering page ownership.
  fixture.Update(2, base + 300 * kMiB, 80);
  fixture.Query(base, 32 * kMiB); // stale registration pages still candidates
  fixture.Query(base + 300 * kMiB, 32 * kMiB); // NOT in registered footprint
  Check(fixture.hybrid.HasValidSnapshot(), "effective-range mutation retains snapshot");

  fixture.Unregister(3);
  Check(!fixture.hybrid.HasValidSnapshot(), "unregister invalidates snapshot");
  fixture.Query(base, 32 * kMiB);
  fixture.Query(base, 32 * kMiB);
  fixture.Query(base, 32 * kMiB);
  Check(fixture.hybrid.SnapshotBuilds() == 2, "lazy rebuild after changed generation");

  fixture.Unregister(4);
  fixture.Register(4, base + 4 * kMiB, 4 * kMiB); // same ID, later registration sequence
  fixture.Query(base, 32 * kMiB);
  fixture.Query(base, 32 * kMiB);
  fixture.Query(base, 32 * kMiB);
  Check(fixture.hybrid.SnapshotBuilds() == 3, "new-generation snapshot after ID reuse");
  fixture.Query(kLimit, 1);
  fixture.Query(kLimit-1, 2);
  fixture.Query(0, 0);
}

void TestH1RandomDifferential() {
  for (bool enabled : {false, true}) {
    HybridImageRangeIndex::Config config;
    config.snapshot_min_pages = 4;
    config.snapshot_after_large_queries = 4;
    config.block_size = 11;
    config.allow_snapshot = enabled;
    Fixture fixture(config);
    uint64_t state = 0x8b37e49dc6731a21ull;
    const uint64_t base = uint64_t{1} << 33;
    for (size_t i = 0; i < 18000; ++i) {
      const auto id = static_cast<uint32_t>(1 + Next(state) % 160);
      const auto address = base + (Next(state) % 4096) * kMiB +
                           (Next(state) % (kMiB - 4096));
      const auto size = 1 + Next(state) % (30 * kMiB);
      switch (Next(state) % 9) {
        case 0: fixture.Register(id, address, size); break;
        case 1: fixture.Unregister(id); break;
        case 2: fixture.Update(id, address, size); break;
        default:
          fixture.Query(address, size);
          if (i % 17 == 0) fixture.Query(base, 1024 * kMiB);
          break;
      }
      if (i % 251 == 0) Check(fixture.hybrid.Validate(), "random invariant");
    }
    Check(fixture.hybrid.Validate(), "final randomized index invariant");
  }
}

} // namespace
int main() {
  TestH0BucketSkipping();
  TestH1LazyLifecycle();
  TestH1RandomDifferential();
  std::puts("ImageRangeHybridTests: H0/H1 deterministic oracle PASS");
  return 0;
}
