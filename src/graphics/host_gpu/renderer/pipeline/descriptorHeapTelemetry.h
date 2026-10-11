#ifndef KYTY_DESCRIPTOR_HEAP_TELEMETRY_H_
#define KYTY_DESCRIPTOR_HEAP_TELEMETRY_H_

// KYTY-INDEX-001 G3-D2. Exact aggregate descriptor-SET accounting, no Vulkan
// dependency, no handles, layout identities, guest addresses, or driver timings.
// Caller serializes recording with DescriptorHeap::Commit on the render thread.
// The producer is compiled out of normal emulator builds by the CMake option.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace Libs::Graphics {
class DescriptorHeapTelemetry final {
public:
  struct Counts {
    uint64_t created_pools = 0;
    uint64_t reset_pools = 0;
    uint64_t retired_pools = 0;
    uint64_t peak_pending_pools = 0;
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    uint64_t allocation_attempts = 0;
    uint64_t rejected_out_of_pool = 0;
    uint64_t rejected_fragmented = 0;
    uint64_t allocated_sets = 0;
    uint64_t issued_sets = 0;
    uint64_t discarded_unused_sets = 0;
    uint64_t peak_cached_unused_sets = 0;
  };

  void PoolCreated() noexcept { ++m_counts.created_pools; }
  void PoolReset() noexcept { ++m_counts.reset_pools; }
  void PoolRetired(uint64_t pending_after) noexcept {
    ++m_counts.retired_pools;
    m_counts.peak_pending_pools = std::max(m_counts.peak_pending_pools, pending_after);
  }
  void CacheHit() noexcept { ++m_counts.cache_hits; }
  void CacheMiss() noexcept { ++m_counts.cache_misses; }
  void AllocateAttempt() noexcept { ++m_counts.allocation_attempts; }
  void AllocateRejected(bool fragmented) noexcept {
    if (fragmented) ++m_counts.rejected_fragmented;
    else ++m_counts.rejected_out_of_pool;
  }
  void Allocated(uint32_t count) noexcept {
    m_counts.allocated_sets += count;
    m_counts.peak_cached_unused_sets =
        std::max(m_counts.peak_cached_unused_sets, CachedUnusedSets());
  }
  void Issued() noexcept { ++m_counts.issued_sets; }
  void DiscardUnused(uint64_t count) noexcept {
    m_counts.discarded_unused_sets += count;
  }
  // Called in parallel with the existing D2 set-count events.
  // Missing/unsupported bindings permanently mark the typed totals incomplete.
  void AllocatedTyped(uint32_t count,const std::array<uint64_t,4>& weight,
                      bool known) noexcept {
    m_typed_seen=true;
    m_typed_allocated_sets+=count;
    if(!known){m_types_complete=false;return;}
    for(size_t i=0;i<4;++i)m_type_allocated[i]+=uint64_t(count)*weight[i];
  }
  void IssuedTyped(const std::array<uint64_t,4>& weight,bool known) noexcept {
    m_typed_seen=true;++m_typed_issued_sets;
    if(!known){m_types_complete=false;return;}
    for(size_t i=0;i<4;++i)m_type_issued[i]+=weight[i];
  }
  void DiscardTyped(uint64_t count,const std::array<uint64_t,4>& totals,
                    bool known) noexcept {
    m_typed_seen=true;m_typed_discarded_sets+=count;
    if(!known){m_types_complete=false;return;}
    for(size_t i=0;i<4;++i)m_type_discarded[i]+=totals[i];
  }
  struct TypeStats {
    std::array<uint64_t,4> allocated{},issued{},discarded{},cached{};
  };
  [[nodiscard]] bool TypedKnown() const noexcept {
    if(!m_typed_seen||!m_types_complete||m_counts.allocated_sets==0||
       m_typed_allocated_sets!=m_counts.allocated_sets||
       m_typed_issued_sets!=m_counts.issued_sets||
       m_typed_discarded_sets!=m_counts.discarded_unused_sets)
      return false;
    for(size_t i=0;i<4;++i) {
      if(m_type_issued[i]>m_type_allocated[i] ||
         m_type_discarded[i]>m_type_allocated[i]-m_type_issued[i])
        return false;
    }
    return true;
  }
  [[nodiscard]] TypeStats TypeSnapshot() const noexcept {
    TypeStats v;
    if(!TypedKnown())return v; // never publish incomplete categories
    v.allocated=m_type_allocated;v.issued=m_type_issued;v.discarded=m_type_discarded;
    for(size_t i=0;i<4;++i)
      v.cached[i]=v.allocated[i]-v.issued[i]-v.discarded[i];
    return v;
  }
  [[nodiscard]] uint64_t CachedUnusedSets() const noexcept {
    if (m_counts.issued_sets > m_counts.allocated_sets ||
        m_counts.discarded_unused_sets > m_counts.allocated_sets - m_counts.issued_sets)
      return 0;
    return m_counts.allocated_sets - m_counts.issued_sets -
           m_counts.discarded_unused_sets;
  }
  [[nodiscard]] bool Valid() const noexcept {
    const auto& c = m_counts;
    if (c.issued_sets > c.allocated_sets ||
        c.discarded_unused_sets > c.allocated_sets - c.issued_sets ||
        c.rejected_out_of_pool > c.allocation_attempts ||
        c.rejected_fragmented > c.allocation_attempts - c.rejected_out_of_pool ||
        c.created_pools == 0 || c.retired_pools < c.reset_pools ||
        c.issued_sets != c.cache_hits + c.cache_misses)
      return false;
    if(m_typed_seen) {
      if(m_typed_allocated_sets!=c.allocated_sets ||
         m_typed_issued_sets!=c.issued_sets ||
         m_typed_discarded_sets!=c.discarded_unused_sets)return false;
      if(m_types_complete)for(size_t i=0;i<4;++i) {
        if(m_type_issued[i]>m_type_allocated[i] ||
           m_type_discarded[i]>m_type_allocated[i]-m_type_issued[i])
          return false;
      }
    }
    return true;
  }
  [[nodiscard]] Counts Snapshot() const noexcept { return m_counts; }

  void WriteCsv(std::FILE* file) const noexcept {
    if (file == nullptr) return;
    std::fputs("metric,value\n", file);
    const auto write = [file](const char* key, uint64_t v) {
      std::fprintf(file, "%s,%llu\n", key, static_cast<unsigned long long>(v));
    };
    const auto& c = m_counts;
    write("schema_version", 1);
    write("pool_creates", c.created_pools);
    write("pool_resets", c.reset_pools);
    write("pool_retires", c.retired_pools);
    write("peak_pending_pools", c.peak_pending_pools);
    write("cache_hits", c.cache_hits);
    write("cache_misses", c.cache_misses);
    write("allocation_attempts", c.allocation_attempts);
    write("reject_out_of_pool", c.rejected_out_of_pool);
    write("reject_fragmented", c.rejected_fragmented);
    write("allocated_sets", c.allocated_sets);
    write("issued_sets", c.issued_sets);
    write("discarded_unused_sets", c.discarded_unused_sets);
    write("currently_cached_unused_sets", CachedUnusedSets());
    write("peak_cached_unused_sets", c.peak_cached_unused_sets);
    write("type_counts_known",uint64_t(TypedKnown()));
    const auto typed=TypeSnapshot();
    static constexpr const char* buckets[]={
      "storage_buffer","sampled_image","storage_image","sampler"
    };
    for(size_t i=0;i<4;++i) {
      std::fprintf(file,"allocated_%s,%llu\n",buckets[i],
                   static_cast<unsigned long long>(typed.allocated[i]));
      std::fprintf(file,"issued_%s,%llu\n",buckets[i],
                   static_cast<unsigned long long>(typed.issued[i]));
      std::fprintf(file,"discarded_%s,%llu\n",buckets[i],
                   static_cast<unsigned long long>(typed.discarded[i]));
      std::fprintf(file,"cached_%s,%llu\n",buckets[i],
                   static_cast<unsigned long long>(typed.cached[i]));
    }
    write("gpu_timing_known", 0);
    write("invariant_pass", uint64_t(Valid()));
    write("production_allocation_policy_modified", 0);
  }
private:
  Counts m_counts;
  std::array<uint64_t,4> m_type_allocated{},m_type_issued{},m_type_discarded{};
  uint64_t m_typed_allocated_sets=0,m_typed_issued_sets=0,m_typed_discarded_sets=0;
  bool m_typed_seen=false,m_types_complete=true;
};
} // namespace Libs::Graphics
#endif // KYTY_DESCRIPTOR_HEAP_TELEMETRY_H_
