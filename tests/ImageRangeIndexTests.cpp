// KYTY-INDEX-001: standalone deterministic differential oracle.
// Cross-repository research log: definitely-stable/Mathlab#289.
#include "graphics/host_gpu/renderer/cache/imageRangeIndex.h"

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
using Libs::Graphics::BlockedImageRangeIndex;
using Libs::Graphics::TreeImageRangeIndex;
constexpr uint64_t kLimit = uint64_t{1} << 44;
constexpr uint64_t kMiB = uint64_t{1} << 20;
constexpr uint64_t kFourK = uint64_t{1} << 12;

void Check(bool success, const char* message) {
  if (!success) {
    std::fprintf(stderr, "ImageRangeIndexTests: %s\n", message);
    std::abort();
  }
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
  LegacyOracle legacy;
  BlockedImageRangeIndex blocked;
  TreeImageRangeIndex tree;
  explicit Fixture(size_t block_size = 64) : blocked(block_size) {}

  void Register(uint32_t id, uint64_t begin, uint64_t size) {
    const bool expected = legacy.Register(id, begin, size);
    Check(blocked.Register(id, begin, size) == expected, "blocked register disagreed");
    Check(tree.Register(id, begin, size) == expected, "AVL register disagreed");
  }
  void Unregister(uint32_t id) {
    const bool expected = legacy.Unregister(id);
    Check(blocked.Unregister(id) == expected, "blocked unregister disagreed");
    Check(tree.Unregister(id) == expected, "AVL unregister disagreed");
  }
  void Update(uint32_t id, uint64_t begin, uint64_t size) {
    const bool expected = legacy.UpdateEffective(id, begin, size);
    Check(blocked.UpdateEffective(id, begin, size) == expected, "blocked update disagreed");
    Check(tree.UpdateEffective(id, begin, size) == expected, "AVL update disagreed");
  }
  void Query(uint64_t begin, uint64_t size) const {
    for (const bool page_overlap : {false, true}) {
      const auto expected = legacy.Find(begin, size, page_overlap);
      const auto left = blocked.Find(begin, size, page_overlap);
      const auto right = tree.Find(begin, size, page_overlap);
      if (left != expected || right != expected) {
        std::fprintf(stderr,
                     "Differential error: address=%llx size=%llx page_overlap=%d"
                     " expected=%zu blocked=%zu AVL=%zu\n",
                     static_cast<unsigned long long>(begin),
                     static_cast<unsigned long long>(size), int(page_overlap),
                     expected.size(), left.size(), right.size());
        std::abort();
      }
    }
  }
  void Validate() const {
    Check(blocked.Validate(), "blocked internal invariant failed");
    Check(tree.Validate(), "AVL height/order/subtree-max invariant failed");
    Check(blocked.Size() == legacy.live.size(), "blocked size mismatch");
    Check(tree.Size() == legacy.live.size(), "AVL size mismatch");
  }
};

uint64_t Next(uint64_t& seed) {
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  return seed;
}

void TestBoundariesAndOrdering() {
  Fixture f(3);
  f.Register(1, kMiB - 2, 4); // two ownership pages
  f.Register(2, kMiB + 5, 25);
  f.Register(3, 2 * kMiB - 1, 2);
  f.Register(4, 3 * kMiB, 1);
  f.Query(kMiB - 1, 2);
  f.Query(kMiB + 1, kMiB);
  f.Query(kMiB - 10, 4 * kMiB);
  f.Query(kMiB + 30, 1); // no byte overlap but 4K page overlaps
  f.Query(0, 1);
  f.Query(0, 0);
  f.Query(kLimit, 1);
  f.Query(kLimit - 1, 2);
  f.Register(5, kLimit - 1, 1);
  f.Query(kLimit - 1, 1);
  f.Query(kLimit - 4096, 4096);
  f.Update(1, 5 * kMiB + 7, 12); // still indexed by original pages
  f.Query(5 * kMiB + 7, 12);
  f.Query(kMiB, 4 * kMiB);
  f.Unregister(2);
  f.Register(2, kMiB - 4, 16); // same ImageId, new registration order
  f.Query(kMiB - 10, 4 * kMiB);
  f.Register(1, 2 * kMiB, 33); // duplicate register rejected
  f.Unregister(42);
  f.Update(42, 0, 1);
  f.Validate();
}

void TestDenseOverlapsAndRotations() {
  for (const size_t block : {size_t{1}, size_t{7}, size_t{64}}) {
    Fixture f(block);
    for (uint32_t id = 1; id <= 256; ++id) {
      const uint64_t start = 9 * kMiB + (id % 7) * 4;
      f.Register(id, start, 2 * kMiB + id);
    }
    f.Query(9 * kMiB, 4 * kMiB);
    f.Query(10 * kMiB + 4095, 2);
    for (uint32_t id = 256; id != 0; id -= 2) {
      f.Unregister(id);
    }
    f.Validate();
    f.Query(9 * kMiB, 4 * kMiB);
    for (uint32_t id = 1; id <= 256; ++id) f.Unregister(id);
    f.Validate();
    f.Query(9 * kMiB, 4 * kMiB);
  }
}

void TestDeterministicDifferential() {
  Fixture f(32);
  uint64_t seed = 0x741d'5f30'26c7'e019ull;
  for (size_t i = 0; i < 25000; ++i) {
    const uint32_t id = 1 + static_cast<uint32_t>(Next(seed) % 200);
    const uint64_t location = Next(seed) % 80;
    const uint64_t begin = ((location % 11) * 64 + location / 11) * kMiB +
                           (Next(seed) % (kMiB - 8192));
    const uint64_t size = 1 + Next(seed) % (4 * kMiB);
    const uint32_t operation = static_cast<uint32_t>(Next(seed) % 7);
    if (operation == 0) f.Register(id, begin, size);
    else if (operation == 1) f.Unregister(id);
    else if (operation == 2) f.Update(id, begin, size);
    else {
      f.Query(begin, size);
      f.Query(begin + size - 1, 1);
    }
    if (i % 199 == 0) f.Validate();
  }
  f.Validate();
  for (const auto& entry : f.legacy.live) f.Query(entry.second.registered_begin,
                                                  entry.second.registered_end -
                                                  entry.second.registered_begin);
}

} // namespace

int main() {
  TestBoundariesAndOrdering();
  TestDenseOverlapsAndRotations();
  TestDeterministicDifferential();
  std::puts("ImageRangeIndexTests: 25000 differential operations PASS");
  return 0;
}
