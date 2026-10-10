#ifndef KYTY_GRAPHICS_NATIVE_LAZY_RANGE_SIDECAR_H_
#define KYTY_GRAPHICS_NATIVE_LAZY_RANGE_SIDECAR_H_

// KYTY-INDEX-001 S4-B4 research: native SlotId-aware, read-only lazy snapshot.
// Contains NO duplicate page-owner table. The caller owns the canonical
// MultiLevelPageTable and calls it when TryFind returns false.
//
// Not wired into production TextureCache. All methods require external
// serialization (as with TextureCache::m_lock). Match callbacks must evaluate
// live Image::Overlaps() / live image metadata and reject stale IDs.
#include "common/slotVector.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class NativeLazyRangeSidecar final {
public:
  using Id = Common::SlotId;
  using Output = InlinePageOwnerList<Id, 16>;

  struct Config {
    uint32_t min_query_pages = 8;
    uint32_t build_after_large_queries = 8;
    size_t block_size = 64;
    size_t max_snapshot_payload_bytes = 1024 * 1024;
  };
  struct Stats {
    size_t inspected = 0;
    size_t candidates = 0;
    bool built = false;
  };

  NativeLazyRangeSidecar() : NativeLazyRangeSidecar(Config{}) {}
  explicit NativeLazyRangeSidecar(Config config) : m_config(config) {
    if (m_config.min_query_pages == 0) m_config.min_query_pages = 1;
    if (m_config.build_after_large_queries == 0) m_config.build_after_large_queries = 1;
    if (m_config.block_size == 0) m_config.block_size = 1;
  }

  [[nodiscard]] static bool Valid(uint64_t address, uint64_t bytes) noexcept {
    return bytes != 0 && address < kAddressLimit && bytes <= kAddressLimit - address;
  }

  // Called only after successful canonical page registration.
  [[nodiscard]] bool Register(Id id, uint64_t address, uint64_t bytes) {
    if (!id || !Valid(address, bytes) || m_live.contains(id) ||
        m_next_sequence == std::numeric_limits<uint64_t>::max()) return false;
    Record rec{};
    rec.id = id;
    rec.first_page = static_cast<uint32_t>(address >> kOwnershipPageShift);
    rec.last_page = static_cast<uint32_t>(((address + bytes - 1) >> kOwnershipPageShift) + 1);
    rec.sequence = m_next_sequence++;
    m_live.emplace(id, rec);
    Invalidate();
    return true;
  }

  [[nodiscard]] bool Unregister(Id id) {
    if (m_live.erase(id) == 0) return false;
    Invalidate();
    return true;
  }

  // Returns true ONLY if the native snapshot answered this query. If false,
  // output is untouched and caller MUST use the canonical page table.
  // A snapshot contains original registered footprint; the match callback
  // reads current effective ranges, including stencil reassociations.
  template <typename Matches>
  [[nodiscard]] bool TryFind(uint64_t address, uint64_t bytes, bool page_overlap,
                             Matches&& matches, Output& output,
                             Stats* stats = nullptr) {
    if (stats) *stats = {};
    if (!Valid(address, bytes)) return false;
    const uint32_t query_first = static_cast<uint32_t>(address >> kOwnershipPageShift);
    const uint32_t query_last =
        static_cast<uint32_t>(((address + bytes - 1) >> kOwnershipPageShift) + 1);
    if (query_last - query_first < m_config.min_query_pages) return false;

    if (!m_snapshot_valid || m_snapshot_generation != m_generation) {
      if (m_cap_rejected) return false;
      if (++m_large_queries < m_config.build_after_large_queries) return false;
      if (!Build()) return false;
      if (stats) stats->built = true;
    }

    m_hits.clear();
    for (size_t group = 0; group < m_block_max.size(); ++group) {
      const size_t first = group * m_config.block_size;
      if (m_snapshot[first].first_page >= query_last) break;
      if (m_block_max[group] <= query_first) continue;
      const size_t last = std::min(m_snapshot.size(), first + m_config.block_size);
      for (size_t pos = first; pos < last && m_snapshot[pos].first_page < query_last; ++pos) {
        const auto& registered = m_snapshot[pos];
        if (stats) ++stats->inspected;
        if (registered.last_page <= query_first) continue;
        const auto current = m_live.find(registered.id);
        if (current == m_live.end() || current->second.sequence != registered.sequence) {
          continue;
        }
        if (matches(registered.id, address, bytes, page_overlap)) {
          m_hits.push_back(
              {std::max(query_first, registered.first_page), registered.sequence, registered.id});
        }
      }
    }
    if (stats) stats->candidates = m_hits.size();
    std::sort(m_hits.begin(), m_hits.end(), [](const Hit& a, const Hit& b) {
      return a.first_hit < b.first_hit ||
             (a.first_hit == b.first_hit && a.sequence < b.sequence);
    });

    Output result;
    for (const auto& hit : m_hits) result.push_back(hit.id);
    output = std::move(result);
    return true;
  }

  [[nodiscard]] uint64_t Generation() const { return m_generation; }
  [[nodiscard]] bool HasValidSnapshot() const {
    return m_snapshot_valid && m_snapshot_generation == m_generation;
  }
  [[nodiscard]] size_t SnapshotBuilds() const { return m_builds; }
  [[nodiscard]] size_t CapDenials() const { return m_cap_denials; }
  [[nodiscard]] size_t Size() const { return m_live.size(); }
  [[nodiscard]] size_t NominalPayloadBytes() const {
    return m_snapshot.capacity() * sizeof(Record) +
           m_block_max.capacity() * sizeof(uint32_t) +
           m_hits.capacity() * sizeof(Hit);
  }
  [[nodiscard]] bool Validate() const {
    if (!HasValidSnapshot()) return !m_snapshot_valid;
    if (m_snapshot.size() != m_live.size() ||
        m_block_max.size() != (m_snapshot.size() + m_config.block_size - 1) /
                                m_config.block_size) return false;
    for (size_t i = 0; i < m_snapshot.size(); ++i) {
      const auto& rec = m_snapshot[i];
      const auto it = m_live.find(rec.id);
      if (it == m_live.end() || it->second.sequence != rec.sequence ||
          it->second.first_page != rec.first_page || it->second.last_page != rec.last_page)
        return false;
      if (i > 0 && !Earlier(m_snapshot[i - 1], rec)) return false;
    }
    for (size_t group = 0; group < m_block_max.size(); ++group) {
      uint32_t max_page = 0;
      for (size_t pos = group * m_config.block_size;
           pos < std::min(m_snapshot.size(), (group + 1) * m_config.block_size); ++pos)
        max_page = std::max(max_page, m_snapshot[pos].last_page);
      if (m_block_max[group] != max_page) return false;
    }
    return true;
  }

private:
  static constexpr uint64_t kAddressLimit = uint64_t{1} << 44;
  static constexpr uint64_t kOwnershipPageShift = 20;
  struct Record {
    Id id{};
    uint32_t first_page = 0;
    uint32_t last_page = 0; // exclusive
    uint64_t sequence = 0;
  };
  struct Hit {
    uint32_t first_hit = 0;
    uint64_t sequence = 0;
    Id id{};
  };
  static bool Earlier(const Record& a, const Record& b) {
    return a.first_page < b.first_page ||
           (a.first_page == b.first_page && a.sequence < b.sequence);
  }
  void Invalidate() {
    m_snapshot_valid = false;
    m_cap_rejected = false;
    m_large_queries = 0;
    if (++m_generation == 0) m_generation = 1;
  }
  [[nodiscard]] bool Build() {
    const size_t count = m_live.size();
    const size_t groups = count / m_config.block_size +
                          (count % m_config.block_size != 0 ? 1 : 0);
    constexpr size_t per_record = sizeof(Record) + sizeof(Hit);
    if (groups > std::numeric_limits<size_t>::max() / sizeof(uint32_t) ||
        count > (std::numeric_limits<size_t>::max() - groups * sizeof(uint32_t)) /
                     per_record ||
        count * per_record + groups * sizeof(uint32_t) >
            m_config.max_snapshot_payload_bytes) {
      m_cap_rejected = true;
      ++m_cap_denials;
      return false;
    }
    m_snapshot.clear();
    m_snapshot.reserve(count);
    for (const auto& pair : m_live) m_snapshot.push_back(pair.second);
    std::sort(m_snapshot.begin(), m_snapshot.end(), Earlier);
    m_hits.clear();
    m_hits.reserve(count);
    m_block_max.clear();
    m_block_max.reserve(groups);
    for (size_t start = 0; start < count; start += m_config.block_size) {
      uint32_t max_page = 0;
      for (size_t i = start; i < std::min(count, start + m_config.block_size); ++i)
        max_page = std::max(max_page, m_snapshot[i].last_page);
      m_block_max.push_back(max_page);
    }
    m_snapshot_generation = m_generation;
    m_snapshot_valid = true;
    ++m_builds;
    return true;
  }

  Config m_config;
  std::unordered_map<Id, Record> m_live;
  std::vector<Record> m_snapshot;
  std::vector<uint32_t> m_block_max;
  std::vector<Hit> m_hits;  // reused scratch; only valid under caller's lock
  uint64_t m_generation = 1;
  uint64_t m_next_sequence = 1;
  uint64_t m_snapshot_generation = 0;
  size_t m_large_queries = 0;
  size_t m_builds = 0;
  size_t m_cap_denials = 0;
  bool m_cap_rejected = false;
  bool m_snapshot_valid = false;
};

}  // namespace Libs::Graphics
#endif  // KYTY_GRAPHICS_NATIVE_LAZY_RANGE_SIDECAR_H_
