// KYTY-INDEX-001 G1-B1: Windows CI-only, source-anchored transfer cost model.
// Uses the production RangeSet implementation; BufferCache / Vulkan remain untouched.
// GPU copy volume here is modeled vk::BufferCopy payload, never measured GPU traffic.
#include "graphics/host_gpu/rangeSet.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using Libs::Graphics::RangeSet;
constexpr uint64_t Base = 0x100000, Length = 512;
constexpr std::array<uint64_t, 5> Gaps {0, 16, 32, 64, 128};
constexpr uint64_t OverfetchBudget = 64;
struct Span { uint64_t begin, end; };
struct Copy { uint64_t begin, end, staging; };
struct Plan {
  std::vector<Copy> copies;
  uint64_t copied = 0, packed = 0, payload = 0, overfetch = 0;
};
struct Totals {
  uint64_t trials = 0, dirty = 0, copied = 0, packed = 0, regions = 0, extra = 0;
  void Add(const Plan& p) {
    ++trials;
    dirty += p.payload; copied += p.copied;
    packed += p.packed; regions += p.copies.size(); extra += p.overfetch;
  }
};
uint64_t checks = 0;
void Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "G1-B1 FAIL: %s check=%llu\n", reason,
                 static_cast<unsigned long long>(checks));
    std::abort();
  }
}
uint64_t Align64(uint64_t size) { return (size + 63) & ~uint64_t{63}; }
std::vector<Span> RealRanges(const RangeSet& ranges, uint64_t begin, uint64_t end) {
  std::vector<Span> result;
  ranges.ForEachInRange(begin, end - begin,
                        [&](uint64_t first, uint64_t last) {
                          result.push_back({first, last});
                        });
  return result;
}
Plan Build(const std::vector<Span>& dirty, uint64_t max_gap, uint64_t budget) {
  Plan plan;
  for (const auto& span: dirty) plan.payload += span.end - span.begin;
  if (dirty.empty()) return plan;
  Span next = dirty.front();
  auto emit = [&] {
    plan.copies.push_back({next.begin, next.end, plan.packed});
    const auto bytes = next.end - next.begin;
    plan.copied += bytes;
    plan.packed += Align64(bytes);  // Real BufferCache uses AlignUp(bytes, 64).
  };
  uint64_t extra = 0;
  for (size_t i = 1; i < dirty.size(); ++i) {
    const auto& r = dirty[i];
    const auto gap = r.begin - next.end;  // The real RangeSet emits sorted disjoint spans.
    if (gap <= max_gap && gap <= budget - extra) {
      next.end = r.end;
      extra += gap;
    } else {
      emit();
      next = r;
    }
  }
  emit();
  plan.overfetch = extra;
  return plan;
}
// Independent byte-domain auditor (rather than reusing the candidate's spans).
bool Valid(const Plan& p, const std::array<bool, Length>& expected,
           uint64_t begin, uint64_t end, uint64_t budget, bool no_overfetch) {
  if (begin < Base || end > Base + Length || end <= begin) return false;
  uint64_t offset = 0, payload = 0, copied = 0, extra = 0, prev_end = begin;
  std::array<bool, Length> covered {};
  for (const auto& c: p.copies) {
    if (c.begin < begin || c.end > end || c.begin >= c.end ||
        c.begin < prev_end || c.staging != offset || (c.staging & 63) != 0) return false;
    const auto bytes = c.end - c.begin;
    if (bytes > UINT64_MAX - 63 || copied > UINT64_MAX - bytes ||
        offset > UINT64_MAX - Align64(bytes)) return false;
    for (uint64_t pos = c.begin; pos < c.end; ++pos) {
      const auto index = static_cast<size_t>(pos - Base);
      if (covered[index]) return false;
      covered[index] = true;
      if (!expected[index]) ++extra;
    }
    copied += bytes;
    offset += Align64(bytes);
    prev_end = c.end;
  }
  for (uint64_t pos = begin; pos < end; ++pos) {
    const auto index = static_cast<size_t>(pos - Base);
    if (expected[index]) {
      ++payload;
      if (!covered[index]) return false;
    }
  }
  if (extra > budget || (no_overfetch && extra != 0)) return false;
  return copied >= payload && copied - payload == extra &&
         p.payload == payload && p.copied == copied &&
         p.packed == offset && p.overfetch == extra;
}
uint32_t state = 0x6a09e667u;
uint32_t Random() {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}
void Csv(const char* label, uint64_t threshold, const Totals& t) {
  std::printf("%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", label,
    static_cast<unsigned long long>(threshold),
    static_cast<unsigned long long>(t.trials),
    static_cast<unsigned long long>(t.dirty),
    static_cast<unsigned long long>(t.copied),
    static_cast<unsigned long long>(t.packed),
    static_cast<unsigned long long>(t.regions),
    static_cast<unsigned long long>(t.extra));
}
void Examples() {
  RangeSet real;
  real.Add(Base, 8);
  real.Add(Base + 16, 8);
  const auto spans = RealRanges(real, Base, Base + 32);
  const auto old_plan = Build(spans, 0, 0);
  const auto proposed = Build(spans, 16, OverfetchBudget);
  Check(old_plan.copies.size() == 2 && old_plan.copied == 16 &&
        old_plan.packed == 128, "real readback packing baseline");
  Check(proposed.copies.size() == 1 && proposed.copied == 24 &&
        proposed.packed == 64 && proposed.overfetch == 8,
        "fewer copies and staging, MORE copied bytes");
  const auto rejected = Build(spans, 4, OverfetchBudget);
  Check(rejected.copies.size() == 2 && rejected.overfetch == 0,
        "gap restriction rejects overfetch");
  std::array<bool, Length> expected {};
  for (uint64_t i = 0; i < 8; ++i) {
    expected[i] = true;
    expected[16 + i] = true;
  }
  Check(Valid(old_plan, expected, Base, Base + 32, 0, true), "baseline example");
  Check(Valid(proposed, expected, Base, Base + 32, OverfetchBudget, false),
        "candidate example");
  auto missing = old_plan;
  missing.copies.pop_back();
  Check(!Valid(missing, expected, Base, Base + 32, 0, true),
        "mutation: missing copied dirty bytes rejected");
  auto alias = old_plan;
  alias.copies[1].staging = alias.copies[0].staging;
  Check(!Valid(alias, expected, Base, Base + 32, 0, true),
        "mutation: staging overlap rejected");
  auto invisible_extra = proposed;
  invisible_extra.overfetch = 0;
  Check(!Valid(invisible_extra, expected, Base, Base + 32, OverfetchBudget, false),
        "mutation: hidden overfetch rejected");
  auto out_of_bounds = proposed;
  out_of_bounds.copies[0].end = Base + 33;
  Check(!Valid(out_of_bounds, expected, Base, Base + 32, OverfetchBudget, false),
        "mutation: outside requested bounds rejected");
}
void Replay() {
  Totals baseline;
  std::array<Totals, Gaps.size()> alternatives {};
  constexpr size_t Trials = 2048;
  for (size_t trial = 0; trial < Trials; ++trial) {
    RangeSet ranges;
    std::array<bool, Length> truth {};
    const size_t ops = 16 + Random() % 32;
    for (size_t j = 0; j < ops; ++j) {
      const uint64_t first = Random() % Length;
      const uint64_t bytes = 1 + Random() % (Length - first);
      const bool insert = (Random() % 3) != 0;
      if (insert) ranges.Add(Base + first, bytes);
      else ranges.Subtract(Base + first, bytes);
      for (uint64_t k = first; k < first + bytes; ++k)
        truth[static_cast<size_t>(k)] = insert;
    }
    const uint64_t from = Random() % Length;
    const uint64_t until = from + 1 + Random() % (Length - from);
    const auto dirty = RealRanges(ranges, Base + from, Base + until);
    const auto original = Build(dirty, 0, 0);
    Check(Valid(original, truth, Base + from, Base + until, 0, true),
          "actual RangeSet dirty-byte differential");
    baseline.Add(original);
    for (size_t i = 0; i < Gaps.size(); ++i) {
      const auto candidate = Build(dirty, Gaps[i], OverfetchBudget);
      Check(Valid(candidate, truth, Base + from, Base + until,
                  OverfetchBudget, false), "hypothetical bounded-gap differential");
      if (Gaps[i] == 0)
        Check(candidate.copies.size() == original.copies.size() &&
              candidate.copied == original.copied &&
              candidate.packed == original.packed &&
              candidate.overfetch == 0,
              "threshold=0 is canonical baseline");
      alternatives[i].Add(candidate);
    }
  }
  std::puts("scenario,max_clean_gap,trials,dirty_payload_bytes,modeled_gpu_copy_bytes,staging_bytes,copy_regions,clean_overfetch_bytes");
  Csv("current_source_model", 0, baseline);
  for (size_t i = 0; i < Gaps.size(); ++i)
    Csv("hypothetical_gap_merge", Gaps[i], alternatives[i]);
  std::printf("summary,checks,%llu,seed,0x6a09e667,trials,%llu,production_changes,0,decision,MODEL_ONLY\n",
              static_cast<unsigned long long>(checks),
              static_cast<unsigned long long>(Trials));
}
} // namespace
int main() {
  Examples();
  Replay();
  return 0;
}
