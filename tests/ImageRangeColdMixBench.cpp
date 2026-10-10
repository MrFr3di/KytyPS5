// KYTY-INDEX-001 S4-B: model the *fraction of lookups reaching* the index.
// Warm descriptor hits are modeled as suppression before Find(), not as a real
// descriptor cache or proof of an in-game hit rate. No renderer changes.
#include "graphics/host_gpu/renderer/cache/imageRangeHybrid.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using Libs::Graphics::HybridImageRangeIndex;
using Clock = std::chrono::steady_clock;
constexpr uint64_t MiB = uint64_t{1} << 20;
constexpr uint64_t GiB = uint64_t{1} << 30;
struct Query { uint64_t address; uint64_t size; bool page_overlap; bool cold; };
struct Workload {
  bool sparse;
  uint32_t cold_percent, churn_interval;
  std::vector<Query> queries;
};
struct Result {
  uint64_t total_ns = 0;
  uint64_t query_ns = 0;
  uint64_t update_ns = 0;
  size_t cold_calls = 0;
  size_t snapshot_builds = 0;
  size_t visited_pages = 0;
  size_t visited_snapshot = 0;
  size_t estimated_bytes = 0;
  std::vector<uint64_t> signatures;
};
uint64_t Random(uint64_t& seed) {
  seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
  return seed;
}
uint64_t Elapsed(Clock::time_point begin) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
}
uint64_t Signature(const HybridImageRangeIndex::Owners& ids) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t id : ids) { h ^= id; h *= 1099511628211ull; }
  return h ^ (static_cast<uint64_t>(ids.size()) << 32);
}
Workload Generate(bool sparse, uint32_t cold_percent, uint32_t churn_interval) {
  Workload w{sparse, cold_percent, churn_interval, {}};
  uint64_t seed = 0x67f0439c21ae5c1bull + uint64_t{sparse} * 41 +
                  cold_percent * 503 + churn_interval * 19;
  w.queries.reserve(400);
  const auto base = uint64_t{2} * GiB;
  for (size_t i = 0; i < 400; ++i) {
    const uint32_t id = static_cast<uint32_t>(Random(seed) % 128);
    uint64_t address = sparse
        ? base + uint64_t{id % 48} * 2 * GiB + uint64_t{id / 48} * 4096
        : base + uint64_t{id % 64} * MiB + uint64_t{id / 64} * 4096;
    address += Random(seed) % 4096;
    const uint64_t size = sparse ? 4 * GiB : 64 * MiB;
    const bool page_overlap = (Random(seed) % 4) == 0;
    const bool cold = Random(seed) % 100 < cold_percent;
    w.queries.push_back({address, size, page_overlap, cold});
  }
  return w;
}
Result Replay(const Workload& w, bool hybrid) {
  HybridImageRangeIndex::Config c;
  c.allow_snapshot = hybrid;
  c.skip_empty_buckets = hybrid;
  c.snapshot_min_pages = 8;
  c.snapshot_after_large_queries = 8;
  HybridImageRangeIndex index(c);
  Result result;
  result.signatures.reserve(w.queries.size());
  const auto start = Clock::now();
  const auto initial = Clock::now();
  const auto base = uint64_t{2} * GiB;
  for (uint32_t id = 1; id <= 128; ++id) {
    const uint64_t address = w.sparse
        ? base + uint64_t{(id-1)%48} * 2 * GiB + uint64_t{(id-1)/48} * 4096
        : base + uint64_t{(id-1)%64} * MiB + uint64_t{(id-1)/64} * 4096;
    const uint64_t len = w.sparse ? 4096 : ((id%7==0) ? 24*MiB : MiB);
    if (!index.Register(id, address, len)) std::abort();
  }
  result.update_ns += Elapsed(initial);
  for (size_t i = 0; i < w.queries.size(); ++i) {
    if (w.churn_interval && i && (i % w.churn_interval) == 0) {
      const uint32_t id = static_cast<uint32_t>(i/w.churn_interval % 128 + 1);
      const uint64_t address = w.sparse
          ? base + uint64_t{(id-1)%48} * 2 * GiB + uint64_t{(id-1)/48} * 4096
          : base + uint64_t{(id-1)%64} * MiB + uint64_t{(id-1)/64} * 4096;
      const auto begin = Clock::now();
      if (!index.Unregister(id) ||
          !index.Register(id, address, w.sparse ? 4096 : ((id%7==0)?24*MiB:MiB))) {
        std::abort();
      }
      result.update_ns += Elapsed(begin);
    }
    const auto& query = w.queries[i];
    if (!query.cold) {
      // This request was already answered outside the index by hypothetical
      // descriptor memoization. We do not pretend to model that cache's cost.
      result.signatures.push_back(UINT64_MAX);
      continue;
    }
    ++result.cold_calls;
    HybridImageRangeIndex::QueryStats stats;
    const auto begin = Clock::now();
    auto hits = index.FindInline(query.address, query.size, query.page_overlap, &stats);
    result.query_ns += Elapsed(begin);
    result.visited_pages += stats.visited_pages;
    result.visited_snapshot += stats.visited_snapshot_records;
    result.signatures.push_back(Signature(hits));
  }
  result.total_ns = Elapsed(start);
  result.snapshot_builds = index.SnapshotBuilds();
  result.estimated_bytes = index.StorageEstimate();
  if (!index.Validate()) std::abort();
  return result;
}
void Emit(const Workload& w, int repeat, bool hybrid, const Result& r) {
  std::printf("%s,%u,%u,%d,%s,%llu,%llu,%llu,%zu,%zu,%zu,%zu,%zu\n",
      w.sparse ? "sparse" : "dense", w.cold_percent, w.churn_interval, repeat,
      hybrid ? "H1_after8" : "legacy",
      static_cast<unsigned long long>(r.total_ns),
      static_cast<unsigned long long>(r.query_ns),
      static_cast<unsigned long long>(r.update_ns),
      r.cold_calls, r.snapshot_builds, r.visited_pages,
      r.visited_snapshot, r.estimated_bytes);
}
} // namespace
int main() {
  std::puts("scenario,cold_percent,churn_interval,repetition,variant,trace_ns,query_ns,update_ns,cold_lookups,snapshot_builds,visited_pages,visited_snapshot_records,index_storage_estimate_bytes");
  for (bool sparse : {false, true}) {
    for (uint32_t cold : {5u, 25u, 100u}) {
      for (uint32_t churn : {0u, 16u, 128u}) {
        const auto workload = Generate(sparse, cold, churn);
        for (int repetition = 0; repetition < 5; ++repetition) {
          const auto a = repetition % 2 == 0 ? Replay(workload, false) : Replay(workload, true);
          const auto b = repetition % 2 == 0 ? Replay(workload, true) : Replay(workload, false);
          const auto& legacy = repetition % 2 == 0 ? a : b;
          const auto& hybrid = repetition % 2 == 0 ? b : a;
          if (legacy.signatures != hybrid.signatures ||
              legacy.cold_calls != hybrid.cold_calls) {
            std::fprintf(stderr, "cold-mix H1 result mismatch sparse=%d cold=%u churn=%u\n",
                         int(sparse), cold, churn);
            return 1;
          }
          Emit(workload, repetition, false, legacy);
          Emit(workload, repetition, true, hybrid);
        }
      }
    }
  }
  return 0;
}
