// KYTY-INDEX-001 S4-B4: independent native SlotId/SlotVector oracle for
// read-only sidecar. Production TextureCache is NOT connected to this class.
#include "graphics/host_gpu/renderer/cache/nativeLazyRangeSidecar.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace {
using Libs::Graphics::NativeLazyRangeSidecar;
using Id = Common::SlotId;
using Owners = NativeLazyRangeSidecar::Output;
using Pages = Libs::Graphics::MultiLevelPageTable<Owners, 20, 44, 14>;
constexpr uint64_t MiB = uint64_t{1} << 20;
constexpr uint64_t KiB4 = uint64_t{1} << 12;
constexpr uint64_t Limit = uint64_t{1} << 44;

void Check(bool ok, const char* where) {
  if (!ok) {
    std::fprintf(stderr, "NativeLazyRangeSidecarTests: %s\n", where);
    std::abort();
  }
}
struct ImageState {
  uint64_t registered_begin, registered_size;
  uint64_t effective_begin, effective_size;
};
bool Matches(const ImageState& image, uint64_t address, uint64_t bytes, bool rounded) {
  const uint64_t end = address + bytes;
  const uint64_t effective_end = image.effective_begin + image.effective_size;
  if (!rounded) return image.effective_begin < end && address < effective_end;
  return (image.effective_begin >> 12) <= ((end - 1) >> 12) &&
         (address >> 12) <= ((effective_end - 1) >> 12);
}
uint64_t Random(uint64_t& state) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}
struct Fixture {
  explicit Fixture(NativeLazyRangeSidecar::Config config = {}) : sidecar(config) {}
  Common::SlotVector<ImageState> images;
  Pages canonical;
  NativeLazyRangeSidecar sidecar;
  std::vector<Id> active;
  size_t answered = 0, fallback = 0;
  Id Register(uint64_t address, uint64_t bytes) {
    Check(NativeLazyRangeSidecar::Valid(address, bytes), "valid registration");
    Id id = images.insert(ImageState{address, bytes, address, bytes});
    Check(sidecar.Register(id, address, bytes), "sidecar register");
    Pages::PageRange pages {};
    Check(Pages::TryGetPageRange(address, bytes, pages), "canonical register range");
    for (size_t i = pages.first; i < pages.last_exclusive; ++i) {
      canonical[i].push_back(id);
    }
    active.push_back(id);
    Check(sidecar.Validate(), "register invariant");
    return id;
  }
  void Unregister(Id id) {
    const auto* image = images.try_get(id);
    Check(image != nullptr, "active registration");
    Pages::PageRange pages {};
    Check(Pages::TryGetPageRange(image->registered_begin, image->registered_size, pages),
          "canonical original range");
    for (size_t i = pages.first; i < pages.last_exclusive; ++i) {
      auto* owners = canonical.Find(i);
      Check(owners && owners->Erase(id), "canonical erase");
    }
    Check(sidecar.Unregister(id), "sidecar unregister");
    Check(!sidecar.Unregister(id), "second unregister must reject stale ID");
    images.erase(id);
    const auto it = std::find(active.begin(), active.end(), id);
    Check(it != active.end(), "remove live reference");
    active.erase(it);
    Check(sidecar.Validate(), "unregister invariant");
  }
  void UpdateEffective(Id id, uint64_t address, uint64_t bytes) {
    auto* image = images.try_get(id);
    Check(image != nullptr && NativeLazyRangeSidecar::Valid(address, bytes),
          "effective mutation");
    image->effective_begin = address;
    image->effective_size = bytes;
    // Sidecar deliberately NOT notified: original registered page footprint
    // is still authoritative, callback reads live current ranges.
  }
  [[nodiscard]] std::vector<Id> Oracle(uint64_t address, uint64_t bytes, bool page_overlap) {
    if (!NativeLazyRangeSidecar::Valid(address, bytes)) return {};
    Pages::PageRange pages {};
    Check(Pages::TryGetPageRange(address, bytes, pages), "oracle query range");
    std::unordered_set<Id> seen;
    std::vector<Id> result;
    for (size_t p = pages.first; p < pages.last_exclusive; ++p) {
      const auto* owners = canonical.Find(p);
      if (!owners) continue;
      owners->ForEach([&](Id id) {
        const auto* image = images.try_get(id);
        if (image == nullptr || !seen.insert(id).second) return;
        if (Matches(*image, address, bytes, page_overlap)) result.push_back(id);
      });
    }
    return result;
  }
  bool Query(uint64_t address, uint64_t bytes, bool page_overlap) {
    const auto expected = Oracle(address, bytes, page_overlap);
    Owners output;
    Id sentinel{0xffffffffu, 0xffffffffu};
    output.push_back(sentinel);
    NativeLazyRangeSidecar::Stats stats;
    const bool taken = sidecar.TryFind(
        address, bytes, page_overlap,
        [this](Id id, uint64_t a, uint64_t b, bool rounded) {
          const auto* image = images.try_get(id);
          return image && Matches(*image, a, b, rounded);
        }, output, &stats);
    if (taken) {
      ++answered;
      std::vector<Id> actual;
      for (Id id : output) actual.push_back(id);
      if (actual != expected) {
        std::fprintf(stderr, "native sidecar incorrect order/overlap: expect=%zu got=%zu"
                             " query=%llx size=%llx rounded=%d\n",
                     expected.size(), actual.size(),
                     static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(bytes), int(page_overlap));
        std::abort();
      }
    } else {
      ++fallback;
      Check(output.size() == 1 && output.front() == sentinel,
            "fallback cannot mutate output");
    }
    Check(sidecar.Validate(), "query invariant");
    return taken;
  }
};

void TestNativeLifecycleAndOrdering() {
  NativeLazyRangeSidecar::Config config;
  config.min_query_pages = 4;
  config.build_after_large_queries = 3;
  config.block_size = 3;
  Fixture f(config);
  const uint64_t base = uint64_t{9} << 30;
  Id long_owner = f.Register(base + 4090, 13 * MiB);
  Id short_owner = f.Register(base + 3 * MiB, KiB4);
  Id neighbor = f.Register(base + 4 * MiB, 8 * MiB);
  Check(!f.Query(base, 20 * MiB, false), "first large fallback");
  Check(!f.Query(base, 20 * MiB, true), "second large fallback");
  Check(f.Query(base, 20 * MiB, false), "build when 3rd large");
  Check(f.sidecar.SnapshotBuilds() == 1, "built exactly once");
  Check(f.Query(base + 3 * MiB, 15 * MiB, true), "snapshot already reused");
  f.UpdateEffective(short_owner, base + 500 * MiB, KiB4);
  Check(f.Query(base, 20 * MiB, false), "live effective outside registered range");
  Check(f.Query(base + 499 * MiB, 8 * MiB, true),
        "live effective cannot make unregistered page membership visible");
  f.Unregister(short_owner);
  Check(!f.sidecar.HasValidSnapshot(), "unregister invalidates");
  Id reused = f.Register(base + 3 * MiB, KiB4);
  Check(reused.index == short_owner.index && reused.generation != short_owner.generation,
        "reuse same index new generation");
  Check(!f.sidecar.Unregister(short_owner), "stale generation cannot delete new owner");
  Check(!f.Query(base, 20 * MiB, true), "new generation first lookup fallback");
  Check(!f.Query(base, 20 * MiB, false), "new generation second fallback");
  Check(f.Query(base, 20 * MiB, true), "new generation snapshot");
  Check(f.sidecar.SnapshotBuilds() == 2, "rebuilt with new generation ID");
  Check(!f.Query(base + KiB4, 1, false), "small queries remain canonical");
  Check(!f.Query(Limit - 1, 2, false), "cross-limit invalid fallback");
  Check(!f.Query(0, 0, true), "empty query invalid fallback");
  f.Unregister(reused);
  f.Unregister(neighbor);
  f.Unregister(long_owner);
}

void TestPayloadDenialAndRecovery() {
  NativeLazyRangeSidecar::Config cfg;
  cfg.min_query_pages = 2;
  cfg.build_after_large_queries = 1;
  cfg.max_snapshot_payload_bytes = 160;
  Fixture f(cfg);
  const uint64_t base = uint64_t{1} << 32;
  for (int i = 0; i < 24; ++i) f.Register(base + i * 128, 8 * MiB);
  for (int i = 0; i < 5; ++i) Check(!f.Query(base, 12 * MiB, false), "too large fallback");
  Check(f.sidecar.CapDenials() == 1 && f.sidecar.SnapshotBuilds() == 0,
        "one cap denial per generation");
  while (f.active.size() > 1) f.Unregister(f.active.back());
  Check(f.Query(base, 12 * MiB, true), "smaller generation may build");
  Check(f.sidecar.CapDenials() == 1 && f.sidecar.SnapshotBuilds() == 1,
        "new generation can recover");
}
void TestDenseOverflowAndBoundary() {
  NativeLazyRangeSidecar::Config cfg;
  cfg.min_query_pages = 2;
  cfg.build_after_large_queries = 1;
  Fixture f(cfg);
  const uint64_t top = Limit - 40 * MiB;
  for (uint32_t i = 0; i < 40; ++i)
    f.Register(top + i * 8, 20 * MiB);
  Check(f.Query(top, 35 * MiB, false), "40 owner inline spill");
  Check(f.Query(top, 35 * MiB, true), "page overlap 40 native");
  Check(f.Query(Limit - MiB, MiB, true), "exact highest page");
  for (int i = 0; i < 24; ++i) f.Unregister(f.active.front());
  Check(f.Query(top, 35 * MiB, true), "overflow to inline transition");
}
void TestRandomDifferential() {
  NativeLazyRangeSidecar::Config cfg;
  cfg.min_query_pages = 4;
  cfg.build_after_large_queries = 4;
  cfg.block_size = 7;
  Fixture f(cfg);
  uint64_t seed = 0xeab812345abcd991ull;
  const uint64_t base = uint64_t{2} << 30;
  for (size_t step = 0; step < 12000; ++step) {
    const uint64_t op = Random(seed) % 9;
    if (op < 2 && f.active.size() < 60) {
      uint64_t address = base + (Random(seed) % 48) * MiB +
                         Random(seed) % (MiB - KiB4);
      f.Register(address, 1 + Random(seed) % (8 * MiB));
    } else if (op == 2 && !f.active.empty()) {
      f.Unregister(f.active[Random(seed) % f.active.size()]);
    } else if (op == 3 && !f.active.empty()) {
      auto id = f.active[Random(seed) % f.active.size()];
      f.UpdateEffective(id, base + Random(seed) % (64 * MiB),
                        1 + Random(seed) % (8 * MiB));
    } else {
      uint64_t address = base + (Random(seed) % 64) * MiB +
                         Random(seed) % (MiB - KiB4);
      uint64_t bytes = (Random(seed) % 4 == 0) ? (16 * MiB) :
                       1 + Random(seed) % (8 * MiB);
      f.Query(address, bytes, (Random(seed) & 1) != 0);
    }
  }
  Check(f.answered > 0 && f.fallback > 0, "both H1 and legacy fallback exercised");
}
} // namespace

int main() {
  TestNativeLifecycleAndOrdering();
  TestPayloadDenialAndRecovery();
  TestDenseOverflowAndBoundary();
  TestRandomDifferential();
  std::puts("NativeLazyRangeSidecarTests: 12k native SlotId operations PASS");
  return 0;
}
