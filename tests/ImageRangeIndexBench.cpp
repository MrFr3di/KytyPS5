// KYTY-INDEX-001 CPU experiment: A/B/C on identical deterministic workloads.
// Benchmark measures a standalone legacy *model*, not in-game FPS or a full
// TextureCache with SlotVector and Vulkan. All three include ordering/filtering.
#include "graphics/host_gpu/renderer/cache/imageRangeIndex.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using Libs::Graphics::BlockedImageRangeIndex;
using Libs::Graphics::TreeImageRangeIndex;
using Owners = Libs::Graphics::InlinePageOwnerList<uint32_t, 16>;
using Table = Libs::Graphics::MultiLevelPageTable<Owners, 20, 44, 14>;
constexpr uint64_t MiB = 1024ull * 1024ull;

struct Item { uint32_t id; uint64_t address; uint64_t size; };
struct Query { uint64_t address; uint64_t size; };

uint64_t Random(uint64_t& seed) {
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  return seed;
}

struct LegacyModel {
  struct Record {
    uint64_t address, size;
    mutable uint64_t seen = 0;
  };
  Table table;
  std::unordered_map<uint32_t, Record> records;
  mutable uint64_t epoch = 0;

  void Register(const Item& image) {
    records.emplace(image.id, Record{image.address, image.size});
    const auto first = image.address / MiB;
    const auto last = (image.address + image.size - 1) / MiB;
    for (uint64_t p = first; p <= last; ++p) table[static_cast<size_t>(p)].push_back(image.id);
  }
  void Unregister(const Item& image) {
    const auto first = image.address / MiB;
    const auto last = (image.address + image.size - 1) / MiB;
    for (uint64_t p = first; p <= last; ++p) {
      auto* owners = table.Find(static_cast<size_t>(p));
      if (owners == nullptr || !owners->Erase(image.id)) std::abort();
    }
    records.erase(image.id);
  }
  std::vector<uint32_t> Find(const Query& query, size_t* inspected = nullptr) const {
    if (inspected) *inspected = 0;
    std::vector<uint32_t> found;
    const auto first = query.address / MiB;
    const auto last = (query.address + query.size - 1) / MiB;
    const auto now = ++epoch;
    for (uint64_t p = first; p <= last; ++p) {
      const auto* owners = table.Find(static_cast<size_t>(p));
      if (owners == nullptr) continue;
      owners->ForEach([&](uint32_t id) {
        if (inspected) ++*inspected;
        const auto it = records.find(id);
        if (it == records.end() || it->second.seen == now) return;
        it->second.seen = now;
        if (it->second.address < query.address + query.size &&
            query.address < it->second.address + it->second.size) found.push_back(id);
      });
    }
    return found;
  }
  size_t StorageEstimate() const {
    return table.AllocatedBucketCount() * size_t{Table::kBucketEntries} * sizeof(Owners) +
           Table::kFirstLevelEntries * sizeof(void*) +
           records.size() * (sizeof(Record) + sizeof(uint32_t) + 2 * sizeof(void*));
  }
};

struct Workload {
  const char* name;
  std::vector<Item> items;
  std::vector<Query> queries;
};

Workload Generate(int scenario) {
  const char* names[] = {"small", "wide", "sparse", "dense", "churn"};
  Workload w{names[scenario], {}, {}};
  uint64_t seed = 0x1de9'713a'bba6'024dull + scenario * 0x10001;
  constexpr uint64_t base = uint64_t{1} << 32;
  const uint32_t count = scenario == 4 ? 256 : 384;
  for (uint32_t id = 1; id <= count; ++id) {
    uint64_t address = 0, size = 0;
    switch (scenario) {
      case 0: // small: many isolated small images over 128 MiB
        address = base + (id % 128) * MiB + (id / 128) * 4096;
        size = 16 * 1024 + Random(seed) % (MiB / 2);
        break;
      case 1: // wide: 32-64 MiB images overlapping their neighbors
        address = base + (id % 60) * MiB;
        size = 32 * MiB + Random(seed) % (32 * MiB);
        break;
      case 2: // sparse: distant populated GiB buckets
        address = base + uint64_t{id % 96} * 1024 * MiB + (id / 96) * 4096;
        size = 4096 + Random(seed) % (MiB / 4);
        break;
      case 3: // dense: many same-page owners
        address = base + (Random(seed) % (MiB - 8192));
        size = 8192 + Random(seed) % (MiB / 8);
        break;
      default: // churn: mixed sizes, repeatedly update the index
        address = base + (id % 80) * MiB;
        size = MiB + Random(seed) % (8 * MiB);
        break;
    }
    w.items.push_back({id, address, size});
  }
  for (size_t i = 0; i < 400; ++i) {
    const auto& candidate = w.items[static_cast<size_t>(Random(seed) % w.items.size())];
    const uint64_t address = candidate.address + (Random(seed) % candidate.size);
    uint64_t size = scenario == 0 ? 4096 :
                    scenario == 1 ? 64 * MiB :
                    scenario == 2 ? 16 * MiB :
                    scenario == 3 ? 256 * 1024 : 4 * MiB;
    // Also exercise queries with no overlapping owner.
    if (i % 5 == 0) size = 128 * MiB;
    w.queries.push_back({address, size});
  }
  return w;
}

using Clock = std::chrono::steady_clock;
uint64_t Nanoseconds(Clock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

volatile uint64_t sink = 0;
template <class Index>
void Measure(const Workload& w, const char* name, int repetition, size_t block_size = 64) {
  Index index;
  const auto build_start = Clock::now();
  for (const auto& image : w.items) {
    if constexpr (requires { index.Register(image); }) index.Register(image);
    else if (!index.Register(image.id, image.address, image.size)) std::abort();
  }
  const auto build_ns = Nanoseconds(build_start);
  size_t inspected = 0;
  uint64_t checksum = 0;
  std::vector<uint64_t> query_samples;
  query_samples.reserve(w.queries.size());
  const auto query_start = Clock::now();
  for (const auto& query : w.queries) {
    size_t query_inspected = 0;
    const auto one_start = Clock::now();
    const auto ids = index.Find(query, &query_inspected);
    query_samples.push_back(Nanoseconds(one_start));
    inspected += query_inspected;
    checksum += ids.size();
    if (!ids.empty()) checksum ^= uint64_t{ids.front()} << 17;
  }
  const auto query_ns = Nanoseconds(query_start);
  std::sort(query_samples.begin(), query_samples.end());
  const auto p50 = query_samples[query_samples.size() / 2];
  const auto p95 = query_samples[(query_samples.size() * 95 + 99) / 100 - 1];
  if (w.name == nullptr) std::abort();
  uint64_t churn_ns = 0;
  if (w.name[0] == 'c' && w.name[1] == 'h') {
    const auto churn_start = Clock::now();
    for (const auto& item : w.items) {
      if constexpr (requires { index.Unregister(item); }) index.Unregister(item);
      else if (!index.Unregister(item.id)) std::abort();
    }
    for (const auto& item : w.items) {
      if constexpr (requires { index.Register(item); }) index.Register(item);
      else if (!index.Register(item.id, item.address, item.size)) std::abort();
    }
    churn_ns = Nanoseconds(churn_start);
  }
  sink = sink ^ checksum;
  std::printf("%s,%s,%d,%zu,%llu,%llu,%llu,%zu,%zu,%llu\n",
              w.name, name, repetition, block_size,
              static_cast<unsigned long long>(build_ns),
              static_cast<unsigned long long>(query_ns),
              static_cast<unsigned long long>(churn_ns),
              inspected, index.StorageEstimate(),
              static_cast<unsigned long long>(checksum),
              static_cast<unsigned long long>(p50),
              static_cast<unsigned long long>(p95));
}

struct LegacyAdapter : LegacyModel {
  void Register(const Item& i) { LegacyModel::Register(i); }
  void Unregister(const Item& i) { LegacyModel::Unregister(i); }
  std::vector<uint32_t> Find(const Query& q, size_t* inspected) const {
    return LegacyModel::Find(q, inspected);
  }
};
struct BlockedAdapter : BlockedImageRangeIndex {
  explicit BlockedAdapter() : BlockedImageRangeIndex(64) {}
  void Register(const Item& i) {
    if (!BlockedImageRangeIndex::Register(i.id, i.address, i.size)) std::abort();
  }
  void Unregister(const Item& i) {
    if (!BlockedImageRangeIndex::Unregister(i.id)) std::abort();
  }
  std::vector<uint32_t> Find(const Query& q, size_t* inspected) const {
    return BlockedImageRangeIndex::Find(q.address, q.size, false, inspected);
  }
};
struct TreeAdapter : TreeImageRangeIndex {
  void Register(const Item& i) {
    if (!TreeImageRangeIndex::Register(i.id, i.address, i.size)) std::abort();
  }
  void Unregister(const Item& i) {
    if (!TreeImageRangeIndex::Unregister(i.id)) std::abort();
  }
  std::vector<uint32_t> Find(const Query& q, size_t* inspected) const {
    return TreeImageRangeIndex::Find(q.address, q.size, false, inspected);
  }
};

void ConfirmExactSequences(const Workload& w) {
  LegacyAdapter old;
  BlockedAdapter blocked;
  TreeAdapter tree;
  for (const auto& item : w.items) {
    old.Register(item); blocked.Register(item); tree.Register(item);
  }
  for (const auto& query : w.queries) {
    const auto expected = old.Find(query, nullptr);
    if (blocked.Find(query, nullptr) != expected ||
        tree.Find(query, nullptr) != expected) {
      std::fprintf(stderr, "benchmark trace semantic mismatch: %s\n", w.name);
      std::abort();
    }
  }
}

} // namespace
int main() {
  std::puts("scenario,variant,repetition,block_size,register_ns,query_ns,churn_ns,total_inspected_records,estimated_index_bytes,checksum,query_p50_ns,query_p95_ns");
  for (int scenario = 0; scenario != 5; ++scenario) {
    auto workload = Generate(scenario);
    ConfirmExactSequences(workload);
    // Rotate A/B/C order to dampen warm CPU/cache and noisy-runner bias.
    for (int repetition = 0; repetition != 9; ++repetition) {
      switch (repetition % 3) {
        case 0:
          Measure<LegacyAdapter>(workload, "legacy", repetition);
          Measure<BlockedAdapter>(workload, "blocked64", repetition);
          Measure<TreeAdapter>(workload, "avl", repetition);
          break;
        case 1:
          Measure<TreeAdapter>(workload, "avl", repetition);
          Measure<LegacyAdapter>(workload, "legacy", repetition);
          Measure<BlockedAdapter>(workload, "blocked64", repetition);
          break;
        default:
          Measure<BlockedAdapter>(workload, "blocked64", repetition);
          Measure<TreeAdapter>(workload, "avl", repetition);
          Measure<LegacyAdapter>(workload, "legacy", repetition);
          break;
      }
    }
  }
  return 0;
}
