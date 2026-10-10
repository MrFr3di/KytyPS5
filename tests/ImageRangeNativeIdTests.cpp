// KYTY-INDEX-001 S3-C2: exercise the exact ImageId (SlotId) ABI.
// The standalone interval prototypes use uint32_t test IDs; this specifically
// detects stale generations and 8-byte owner packing in the real page owner list.
#include "common/slotVector.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace {
using Libs::Graphics::ImageId;
using NativeOwners = Libs::Graphics::InlinePageOwnerList<ImageId, 16>;
using NativePages = Libs::Graphics::MultiLevelPageTable<NativeOwners, 20, 44, 14>;

void Check(bool value, const char* why) {
  if (!value) {
    std::fprintf(stderr, "ImageRangeNativeIdTests: %s\n", why);
    std::abort();
  }
}

void TestNativeIdentityAndLifetime() {
  static_assert(std::is_same_v<ImageId, Common::SlotId>);
  static_assert(std::is_trivially_copyable_v<ImageId>);
  static_assert(std::has_unique_object_representations_v<ImageId>);
  static_assert(sizeof(ImageId) == sizeof(uint64_t));

  Common::SlotVector<uint32_t> slots;
  const ImageId first = slots.insert(11u);
  const ImageId neighbor = slots.insert(22u);
  Check(slots.try_get(first) && *slots.try_get(first) == 11, "original ID alive");
  NativePages pages;
  constexpr uint64_t page = 421;
  pages[page].push_back(first);
  pages[page].push_back(neighbor);
  slots.erase(first);
  const ImageId reused = slots.insert(33u);
  Check(reused.index == first.index && reused.generation != first.generation,
        "SlotVector must increment generation on index reuse");
  Check(slots.try_get(first) == nullptr && slots.try_get(reused) != nullptr,
        "old generation must never resolve to a reused slot");
  Check(first != reused && std::hash<ImageId>{}(first) != std::hash<ImageId>{}(reused),
        "generation must participate in hash and equality");
  auto* owners = pages.Find(page);
  Check(owners && owners->Contains(first) && !owners->Contains(reused),
        "packed owners compare both index and generation");
  Check(owners->Erase(first) && !owners->Contains(first),
        "erase stale generation without removing reused image");
  owners->push_back(reused);
  Check(owners->size() == 2 && (*owners)[0] == neighbor && (*owners)[1] == reused,
        "registered owner ordering survives slot reuse");
}

void TestInlineCapacityAndOverflow() {
  Common::SlotVector<uint32_t> slots;
  NativeOwners owners;
  std::vector<ImageId> expected;
  for (uint32_t n = 0; n < 40; ++n) {
    auto id = slots.insert(n);
    owners.push_back(id);
    expected.push_back(id);
  }
  Check(owners.size() == 40, "native ID overflow list supports 40 image IDs");
  for (uint32_t i = 0; i < owners.size(); ++i) {
    Check(owners[i] == expected[i], "inline/overflow owner must preserve 64-bit native ID");
  }
  std::unordered_set<ImageId> unique(owners.begin(), owners.end());
  Check(unique.size() == owners.size(), "hash/iterator should preserve all IDs");
  for (uint32_t i = 0; i < 24; ++i) {
    Check(owners.Erase(expected[i]), "remove owners through overflow-to-inline boundary");
    slots.erase(expected[i]);
  }
  Check(owners.size() == 16, "native list switches to inline at 16");
  for (uint32_t i = 0; i < 16; ++i) {
    Check(owners[i] == expected[24 + i], "overflow-to-inline preserves generation and order");
  }
  for (uint32_t i = 0; i < 24; ++i) {
    auto reused = slots.insert(i + 100);
    Check(!owners.Contains(reused), "generation-specific IDs cannot be falsely matched");
  }
}

void TestCrossPageNativeOwnerOrdering() {
  NativePages pages;
  constexpr uint64_t first = 1023, second = 1024;
  ImageId long_image {3, 5}, later_image {9, 2}, reused {3, 6};
  pages[first].push_back(long_image);
  pages[second].push_back(long_image);
  pages[second].push_back(later_image);
  std::vector<ImageId> observed;
  for (uint64_t p = first; p <= second; ++p) {
    const auto* entries = pages.Find(p);
    if (!entries) continue;
    entries->ForEach([&](ImageId id) {
      if (std::find(observed.begin(), observed.end(), id) == observed.end()) {
        observed.push_back(id);
      }
    });
  }
  Check(observed.size() == 2 && observed[0] == long_image && observed[1] == later_image,
        "first shared page then registration ordering");
  Check(pages[second].Erase(long_image), "exact generation erasure");
  pages[second].push_back(reused);
  Check(pages[second].Contains(reused) && !pages[second].Contains(long_image),
        "reused native slot identity is independent on each page");
}
} // namespace
int main() {
  TestNativeIdentityAndLifetime();
  TestInlineCapacityAndOverflow();
  TestCrossPageNativeOwnerOrdering();
  std::puts("ImageRangeNativeIdTests: native generation/owner storage PASS");
  return 0;
}
