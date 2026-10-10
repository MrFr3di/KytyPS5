#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGERANGEHYBRID_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGERANGEHYBRID_H_

// KYTY-INDEX-001 H0/H1: experimental, standalone CPU-only ownership model.
// This is NOT yet wired into TextureCache. The canonical one-MiB page table
// stays authoritative. Read-only blocked snapshots are built lazily and
// invalidated by registration changes; effective ranges are always read live.
// See definitely-stable/Mathlab#289 for the frozen experiment protocol.
#include "graphics/host_gpu/renderer/cache/imageRangeIndex.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class HybridImageRangeIndex final {
public:
  using Owners = InlinePageOwnerList<uint32_t, 16>;
  using Table = MultiLevelPageTable<Owners, 20, 44, 14>;

  struct Config {
    uint32_t snapshot_min_pages = 8;
    uint32_t snapshot_after_large_queries = 8;
    size_t block_size = 64;
    // Admission budget for snapshot records + block summaries, not heap/RSS.
    size_t max_snapshot_payload_bytes = 1024 * 1024;
    bool allow_snapshot = true;
    bool skip_empty_buckets = true;
  };
  enum class QueryPath : uint8_t { Legacy, BucketSkip, Snapshot };
  struct QueryStats {
    QueryPath path = QueryPath::Legacy;
    size_t visited_pages = 0;
    size_t visited_owners = 0;
    size_t visited_snapshot_records = 0;
    size_t sorted_candidates = 0;
    size_t snapshot_build_records = 0;
  };

  HybridImageRangeIndex() : HybridImageRangeIndex(Config{}) {}
  explicit HybridImageRangeIndex(Config config) : m_config(config) {
    if (!m_config.block_size) m_config.block_size = 64;
    if (!m_config.snapshot_min_pages) m_config.snapshot_min_pages = 1;
    if (!m_config.snapshot_after_large_queries) m_config.snapshot_after_large_queries = 1;
  }

  bool Register(uint32_t id, uint64_t address, uint64_t size) {
    if (!ImageRangeDetail::Valid(address, size) || m_live.contains(id) ||
        m_next_sequence == std::numeric_limits<uint64_t>::max()) return false;
    const ImageRangeRecord record =
        ImageRangeDetail::MakeRecord(id, address, size, m_next_sequence++);
    m_live.emplace(id, Live{record});
    for (size_t p = record.first_page; p < record.last_page; ++p) {
      m_table[p].push_back(id);
    }
    Invalidate();
    return true;
  }
  bool Unregister(uint32_t id) {
    const auto found = m_live.find(id);
    if (found == m_live.end()) return false;
    const auto record = found->second.record;
    for (size_t p = record.first_page; p < record.last_page; ++p) {
      auto* owners = m_table.Find(p);
      if (!owners || !owners->Erase(id)) return false;
    }
    m_live.erase(found);
    Invalidate();
    return true;
  }

  // Model mutations such as AssociateStencil(): legacy's page membership stays
  // tied to registered range; final overlap reads current Image::Overlaps().
  // Since snapshot stores only the frozen membership, no rebuild is needed.
  bool UpdateEffective(uint32_t id, uint64_t address, uint64_t size) {
    if (!ImageRangeDetail::Valid(address, size)) return false;
    const auto found = m_live.find(id);
    if (found == m_live.end()) return false;
    found->second.record.effective_begin = address;
    found->second.record.effective_end = address + size;
    return true;
  }

  // Standalone vector API used by the original research tests.
  [[nodiscard]] std::vector<uint32_t> Find(uint64_t address, uint64_t size,
                                            bool page_overlap = false,
                                            QueryStats* stats = nullptr) {
    return FindTyped<std::vector<uint32_t>>(address, size, page_overlap, stats);
  }

  // Production-equivalent inline return storage: no heap allocation when
  // 0..16 ImageIds are produced on the page-scan path. The snapshot path still
  // materializes and sorts candidate hits and must be charged for that scratch.
  [[nodiscard]] Owners FindInline(uint64_t address, uint64_t size,
                                  bool page_overlap = false,
                                  QueryStats* stats = nullptr) {
    return FindTyped<Owners>(address, size, page_overlap, stats);
  }

  [[nodiscard]] size_t Size() const { return m_live.size(); }
  [[nodiscard]] uint64_t Generation() const { return m_generation; }
  [[nodiscard]] size_t SnapshotBuilds() const { return m_snapshot_builds; }
  [[nodiscard]] size_t SnapshotCapDenials() const { return m_snapshot_cap_denials; }
  [[nodiscard]] bool HasValidSnapshot() const {
    return m_snapshot_valid && m_snapshot_generation == m_generation;
  }
  [[nodiscard]] size_t StorageEstimate() const {
    return m_table.AllocatedBucketCount() * Table::kBucketEntries * sizeof(Owners) +
           Table::kFirstLevelEntries * sizeof(void*) +
           m_live.size() * (sizeof(Live) + sizeof(uint32_t) + 2 * sizeof(void*)) +
           m_snapshot.capacity() * sizeof(ImageRangeRecord) +
           m_block_max.capacity() * sizeof(uint32_t) +
           m_query_hits.capacity() * sizeof(ImageRangeDetail::Hit);
  }

  [[nodiscard]] bool Validate() const {
    if (!HasValidSnapshot()) return !m_snapshot_valid;
    if (m_snapshot.size() != m_live.size() ||
        m_block_max.size() != (m_snapshot.size() + m_config.block_size - 1) /
                                 m_config.block_size) return false;
    for (size_t i = 0; i < m_snapshot.size(); ++i) {
      const auto& rec = m_snapshot[i];
      if (i && !ImageRangeDetail::Earlier(m_snapshot[i-1], rec)) return false;
      const auto it = m_live.find(rec.id);
      if (it == m_live.end() || rec.registered_begin != it->second.record.registered_begin ||
          rec.registered_end != it->second.record.registered_end ||
          rec.sequence != it->second.record.sequence) return false;
    }
    for (size_t b = 0; b < m_block_max.size(); ++b) {
      uint32_t maximum = 0;
      for (size_t i = b*m_config.block_size;
           i < std::min(m_snapshot.size(), (b+1)*m_config.block_size); ++i) {
        maximum = std::max(maximum, m_snapshot[i].last_page);
      }
      if (maximum != m_block_max[b]) return false;
    }
    return true;
  }

private:
  template <typename Output>
  [[nodiscard]] Output FindTyped(uint64_t address, uint64_t size,
                                 bool page_overlap, QueryStats* stats) {
    if (stats) *stats = {};
    if (!ImageRangeDetail::Valid(address, size)) return {};
    const uint64_t end = address + size;
    const uint32_t first = ImageRangeDetail::FirstPage(address);
    const uint32_t last = ImageRangeDetail::LastPage(end);
    const uint32_t pages = last - first;
    if (pages < m_config.snapshot_min_pages) {
      return PageScan<Output>(first, last, address, end, page_overlap, false, stats);
    }
    if (m_config.allow_snapshot && m_snapshot_valid &&
        m_snapshot_generation == m_generation) {
      return SnapshotScan<Output>(first, last, address, end, page_overlap, stats);
    }
    if (m_config.allow_snapshot && !m_snapshot_cap_rejected) {
      ++m_large_queries_since_mutation;
      if (m_large_queries_since_mutation >= m_config.snapshot_after_large_queries) {
        if (TryBuildSnapshot()) {
          if (stats) stats->snapshot_build_records = m_snapshot.size();
          return SnapshotScan<Output>(first, last, address, end, page_overlap, stats);
        }
      }
    }
    return PageScan<Output>(first, last, address, end, page_overlap,
                            m_config.skip_empty_buckets, stats);
  }

  struct Live {
    explicit Live(ImageRangeRecord r) : record(r) {}
    ImageRangeRecord record;
    uint64_t seen = 0;
  };

  void Invalidate() {
    m_snapshot_valid = false;
    m_snapshot_cap_rejected = false;
    m_large_queries_since_mutation = 0;
    if (m_generation == std::numeric_limits<uint64_t>::max()) {
      m_generation = 1;
    } else {
      ++m_generation;
    }
  }
  uint64_t NewEpoch() {
    if (m_query_epoch == std::numeric_limits<uint64_t>::max()) {
      for (auto& item : m_live) item.second.seen = 0;
      m_query_epoch = 0;
    }
    return ++m_query_epoch;
  }
  template <typename Output>
  [[nodiscard]] Output PageScan(uint32_t first, uint32_t last,
                                                 uint64_t address, uint64_t end,
                                                 bool page_overlap, bool skip_buckets,
                                                 QueryStats* stats) {
    if (stats) stats->path = skip_buckets ? QueryPath::BucketSkip : QueryPath::Legacy;
    Output result;
    const auto epoch = NewEpoch();
    auto visit = [&](uint32_t, const Owners& owners) {
      if (stats) ++stats->visited_pages;
      owners.ForEach([&](uint32_t id) {
        if (stats) ++stats->visited_owners;
        const auto it = m_live.find(id);
        if (it == m_live.end() || it->second.seen == epoch) return;
        it->second.seen = epoch;
        if (ImageRangeDetail::Matches(it->second.record, address, end, page_overlap)) {
          result.push_back(id);
        }
      });
    };
    if (skip_buckets) {
      m_table.ForEachAllocatedPageInRange(
          Table::PageRange{first, last},
          [&](size_t page, const Owners& owners) {
            visit(static_cast<uint32_t>(page), owners);
          });
    } else {
      for (size_t page = first; page < last; ++page) {
        const auto* owners = m_table.Find(page);
        if (stats) ++stats->visited_pages;
        if (owners) owners->ForEach([&](uint32_t id) {
          if (stats) ++stats->visited_owners;
          const auto it = m_live.find(id);
          if (it == m_live.end() || it->second.seen == epoch) return;
          it->second.seen = epoch;
          if (ImageRangeDetail::Matches(it->second.record, address, end, page_overlap)) {
            result.push_back(id);
          }
        });
      }
    }
    return result;
  }

  bool TryBuildSnapshot() {
    const size_t count = m_live.size();
    const size_t groups = count / m_config.block_size +
                          (count % m_config.block_size != 0 ? 1 : 0);
    // Check arithmetic before allocation; metadata is charged separately.
    constexpr size_t kPerOwner = sizeof(ImageRangeRecord) +
                                 sizeof(ImageRangeDetail::Hit);
    if (groups > std::numeric_limits<size_t>::max() / sizeof(uint32_t) ||
        count > (std::numeric_limits<size_t>::max() -
                 groups * sizeof(uint32_t)) / kPerOwner ||
        count * kPerOwner +
                groups * sizeof(uint32_t) > m_config.max_snapshot_payload_bytes) {
      m_snapshot_cap_rejected = true;
      ++m_snapshot_cap_denials;
      return false;
    }
    m_snapshot.clear();
    m_snapshot.reserve(count);
    for (const auto& item : m_live) m_snapshot.push_back(item.second.record);
    std::sort(m_snapshot.begin(), m_snapshot.end(), ImageRangeDetail::Earlier);
    // Under the same TextureCache lock, query scratch can be reused safely.
    // The standalone experiment is intentionally single-threaded. Its payload
    // is explicitly charged to the admission budget and storage estimate.
    m_query_hits.clear();
    m_query_hits.reserve(count);
    m_block_max.clear();
    m_block_max.reserve(groups);
    for (size_t base = 0; base < m_snapshot.size(); base += m_config.block_size) {
      uint32_t maximum = 0;
      for (size_t p = base; p < std::min(m_snapshot.size(), base + m_config.block_size); ++p) {
        maximum = std::max(maximum, m_snapshot[p].last_page);
      }
      m_block_max.push_back(maximum);
    }
    m_snapshot_generation = m_generation;
    m_snapshot_valid = true;
    ++m_snapshot_builds;
    return true;
  }

  template <typename Output>
  [[nodiscard]] Output SnapshotScan(uint32_t first, uint32_t last,
                                                    uint64_t address, uint64_t end,
                                                    bool page_overlap, QueryStats* stats) {
    if (stats) stats->path = QueryPath::Snapshot;
    m_query_hits.clear();
    auto& hits = m_query_hits;
    for (size_t block = 0; block < m_block_max.size(); ++block) {
      const size_t start = block * m_config.block_size;
      if (m_snapshot[start].first_page >= last) break;
      if (m_block_max[block] <= first) continue;
      const size_t stop = std::min(m_snapshot.size(), start + m_config.block_size);
      for (size_t p = start; p < stop && m_snapshot[p].first_page < last; ++p) {
        if (stats) ++stats->visited_snapshot_records;
        const auto& registered = m_snapshot[p];
        const auto live = m_live.find(registered.id);
        if (live == m_live.end() || registered.sequence != live->second.record.sequence) {
          continue; // stale/deferred image must never be emitted
        }
        ImageRangeDetail::AddHit(hits, live->second.record, first, last,
                                 address, end, page_overlap);
      }
    }
    if (stats) stats->sorted_candidates = hits.size();
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
      return a.first_hit < b.first_hit ||
             (a.first_hit == b.first_hit && a.sequence < b.sequence);
    });
    Output output;
    for (const auto& hit : hits) output.push_back(hit.id);
    return output;
  }

  Config m_config;
  Table m_table;
  std::unordered_map<uint32_t, Live> m_live;
  std::vector<ImageRangeRecord> m_snapshot;
  std::vector<uint32_t> m_block_max;
  // Reused candidate-sort scratch; queries are serialized by the caller.
  std::vector<ImageRangeDetail::Hit> m_query_hits;
  uint64_t m_next_sequence = 1;
  uint64_t m_generation = 1;
  uint64_t m_snapshot_generation = 0;
  uint64_t m_query_epoch = 0;
  size_t m_large_queries_since_mutation = 0;
  size_t m_snapshot_builds = 0;
  size_t m_snapshot_cap_denials = 0;
  bool m_snapshot_cap_rejected = false;
  bool m_snapshot_valid = false;
};

} // namespace Libs::Graphics
#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGERANGEHYBRID_H_
