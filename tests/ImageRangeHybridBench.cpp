// KYTY-INDEX-001: H0/H1 weighted deterministic CPU trace benchmark.
// The same canonical page index and owner storage are used for all modes.
// Source of truth for conclusions: definitely-stable/Mathlab#289.
// Timings include lazy builds, page scans, ordered sorting and churn updates.
// No game/GPU/FPS result can be inferred from this standalone test.
#include "graphics/host_gpu/renderer/cache/imageRangeHybrid.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using Libs::Graphics::HybridImageRangeIndex;
using Clock = std::chrono::steady_clock;
constexpr uint64_t MiB = uint64_t{1} << 20;
constexpr uint64_t GiB = uint64_t{1} << 30;
struct Item { uint32_t id; uint64_t address; uint64_t size; };
struct Query { uint64_t address; uint64_t size; bool page_overlap; };
struct Workload {
  bool sparse = false;
  uint32_t wide_percent = 0;
  uint32_t churn_interval = 0;
  std::vector<Item> images;
  std::vector<Query> queries;
};
struct Result {
  uint64_t trace_ns = 0;
  uint64_t query_ns = 0;
  uint64_t mutation_ns = 0;
  uint64_t p50_ns = 0;
  uint64_t p95_ns = 0;
  size_t visited_pages = 0;
  size_t visited_owners = 0;
  size_t visited_snapshot_records = 0;
  size_t sorted_candidates = 0;
  size_t build_records = 0;
  size_t snapshot_builds = 0;
  size_t storage_estimate_bytes = 0;
  uint64_t working_set_bytes = 0;
  std::vector<uint64_t> result_signatures;
};
uint64_t Next(uint64_t& seed) {
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  return seed;
}
uint64_t Elapsed(Clock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());
}
uint64_t WorkingSet() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    return static_cast<uint64_t>(counters.WorkingSetSize);
  }
#endif
  return 0;
}
Workload Generate(bool sparse, uint32_t wide_percent, uint32_t churn_interval) {
  Workload w;
  w.sparse = sparse; w.wide_percent = wide_percent;
  w.churn_interval = churn_interval;
  uint64_t seed = 0x936ade157832104full + (sparse?977:0) +
                  wide_percent*17 + churn_interval*71;
  const uint64_t base = 2 * GiB;
  for (uint32_t id = 1; id <= 256; ++id) {
    const uint64_t address = sparse
        ? base + (uint64_t{id % 48} * 2 * GiB) + (id / 48) * 4096
        : base + uint64_t{id % 80} * MiB + (id / 80) * 4096;
    const uint64_t size = sparse
        ? (id % 8 == 0 ? MiB : 4096)
        : ((id % 11 == 0 ? 48 : id % 5 + 1) * MiB);
    w.images.push_back({id, address, size});
  }
  for (size_t i = 0; i < 400; ++i) {
    const auto& item = w.images[static_cast<size_t>(Next(seed) % w.images.size())];
    const bool wide = Next(seed) % 100 < wide_percent;
    const auto address = item.address + Next(seed) % item.size;
    const uint64_t size = wide ? (sparse ? 4 * GiB : 64 * MiB) : 4096;
    w.queries.push_back({address, size, (Next(seed) % 4) == 0});
  }
  return w;
}
uint64_t Signature(const std::vector<uint32_t>& ids) {
  uint64_t value = 1469598103934665603ull;
  for (uint32_t id : ids) {
    value ^= id;
    value *= 1099511628211ull;
  }
  return value ^ (uint64_t(ids.size()) << 32);
}
HybridImageRangeIndex::Config Config(int variant) {
  HybridImageRangeIndex::Config c;
  c.snapshot_min_pages = 8;
  c.snapshot_after_large_queries = variant == 2 ? 8 : variant == 3 ? 16 : 32;
  c.block_size = 64;
  c.allow_snapshot = variant >= 2;
  c.skip_empty_buckets = variant != 0;
  return c;
}
Result Replay(const Workload& workload, int variant) {
  HybridImageRangeIndex index(Config(variant));
  Result result;
  const auto start = Clock::now();
  const auto build_start = Clock::now();
  for (const auto& item : workload.images) {
    if (!index.Register(item.id, item.address, item.size)) std::abort();
  }
  result.mutation_ns += Elapsed(build_start);
  std::vector<uint64_t> latencies;
  latencies.reserve(workload.queries.size());
  result.result_signatures.reserve(workload.queries.size());
  for (size_t i = 0; i < workload.queries.size(); ++i) {
    if (workload.churn_interval != 0 && i != 0 &&
        i % workload.churn_interval == 0) {
      const auto& image = workload.images[(i/workload.churn_interval) % workload.images.size()];
      const auto churn_start = Clock::now();
      if (!index.Unregister(image.id)) std::abort();
      if (!index.Register(image.id, image.address, image.size)) std::abort();
      result.mutation_ns += Elapsed(churn_start);
    }
    HybridImageRangeIndex::QueryStats stats{};
    const auto start_lookup = Clock::now();
    const auto& q = workload.queries[i];
    auto ids = index.Find(q.address, q.size, q.page_overlap, &stats);
    const auto elapsed = Elapsed(start_lookup);
    result.query_ns += elapsed;
    latencies.push_back(elapsed);
    result.result_signatures.push_back(Signature(ids));
    result.visited_pages += stats.visited_pages;
    result.visited_owners += stats.visited_owners;
    result.visited_snapshot_records += stats.visited_snapshot_records;
    result.sorted_candidates += stats.sorted_candidates;
    result.build_records += stats.snapshot_build_records;
  }
  result.trace_ns = Elapsed(start);
  std::sort(latencies.begin(), latencies.end());
  result.p50_ns = latencies[latencies.size()/2];
  result.p95_ns = latencies[(latencies.size()*95 + 99)/100-1];
  result.snapshot_builds = index.SnapshotBuilds();
  result.storage_estimate_bytes = index.StorageEstimate();
  result.working_set_bytes = WorkingSet();
  if (!index.Validate()) std::abort();
  return result;
}
void Emit(const Workload& w, int repetition, int variant, const Result& r) {
  const char* names[] = {"legacy","H0_bucket_skip","H1_after8","H1_after16","H1_after32"};
  std::printf("%s,%u,%u,%d,%s,%llu,%llu,%llu,%llu,%llu,%zu,%zu,%zu,%zu,%zu,%zu,%llu\n",
              w.sparse?"sparse":"dense", w.wide_percent, w.churn_interval, repetition,
              names[variant],
              static_cast<unsigned long long>(r.trace_ns),
              static_cast<unsigned long long>(r.query_ns),
              static_cast<unsigned long long>(r.mutation_ns),
              static_cast<unsigned long long>(r.p50_ns),
              static_cast<unsigned long long>(r.p95_ns),
              r.visited_pages, r.visited_owners, r.visited_snapshot_records,
              r.sorted_candidates, r.build_records, r.snapshot_builds,
              static_cast<unsigned long long>(r.storage_estimate_bytes),
              static_cast<unsigned long long>(r.working_set_bytes));
}
} // namespace

int main() {
  std::puts("scenario,wide_percent,churn_every_queries,repetition,variant,trace_ns,query_ns,mutation_ns,lookup_p50_ns,lookup_p95_ns,visited_pages_total,visited_owners_total,visited_snapshot_records_total,sort_candidates_total,snapshot_build_records_total,snapshot_builds,index_storage_estimate_bytes,process_working_set_bytes");
  for (const bool sparse : {false, true}) {
    for (const uint32_t wide : {0u, 10u, 50u, 100u}) {
      for (const uint32_t churn : {0u, 16u, 128u}) {
        const auto w = Generate(sparse, wide, churn);
        for (int repetition = 0; repetition < 9; ++repetition) {
          Result result[5];
          // Rotate trial order, compare exact ordered fingerprints after timings.
          for (int j = 0; j < 5; ++j) {
            const int variant = (j + repetition) % 5;
            result[variant] = Replay(w, variant);
          }
          if (result[0].result_signatures != result[1].result_signatures ||
              result[0].result_signatures != result[2].result_signatures ||
              result[0].result_signatures != result[3].result_signatures ||
              result[0].result_signatures != result[4].result_signatures) {
            std::fprintf(stderr,
              "Hybrid benchmark differential mismatch sparse=%d wide=%u churn=%u\n",
              int(sparse), wide, churn);
            return 2;
          }
          for (int variant = 0; variant < 5; ++variant) Emit(w, repetition, variant, result[variant]);
        }
      }
    }
  }
  return 0;
}
