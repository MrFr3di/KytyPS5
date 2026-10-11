#ifndef KYTY_TEXTURE_WORKLOAD_ATTRIBUTION_H_
#define KYTY_TEXTURE_WORKLOAD_ATTRIBUTION_H_

// KYTY-INDEX-001 G0: bounded, address-free CPU attribution for texture work.
// Diagnostic only: no GPU timestamps, FPS claim, or call-path changes.
// Probes on the same thread form a stack so that nested work is subtracted
// from the parent's self time. Atomics make cross-thread snapshots race-safe.
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace Libs::Graphics {

class TextureWorkloadAttribution final {
public:
  enum class Stage : size_t {
    RegionLookup, ImageDiscovery, ResolveOverlap, ImageRefresh,
    ImageInitialize, ImageCopy, CpuAliasInvalidate, GpuMemoryInvalidate,
    GarbageCollect, RegisterImage, UnregisterImage, ResolveTexture,
    RebindImages, DescriptorCommit, TextureReadback, ProcessReadbacks,
    BufferDownload, BufferUpload, BufferSynchronize, BufferAcquire, BufferGarbageCollect, Count
  };
  enum class Event : size_t {
    RebindReuse, RebindMissing, RebindUnregistered, RebindNeeded,
    DescriptorPushWrites, DescriptorSetWrites, BufferUploadStagedBytes,
    BufferUploadRegions, BufferUploadBatches, BufferReadbackPackedBytes,
    BufferReadbackPayloadBytes, BufferReadbackRegions,
    TextureReadbackRequestedBytes, ImageUploadRequests, ImageUploadGuestBytes,
    ImageGcChecks, ImageGcBelowTrigger, ImageGcRootCandidates, ImageGcRootRetireRequests,
    ImageGcSkippedTiled, ImageGcSkippedUnpressured, ImageGcSkippedReadback,
    ResourceSnapshots, Count
  };
  enum class Gauge : size_t {
    ImageSlotsLive, ImageSlotsCapacity, ImageSlotsRegistered, NativeImagesLive,
    ImageViewsLive, ImageSlotsPeakObserved, ImageViewsPeakObserved, Count
  };
  struct Sample {
    uint64_t calls = 0;
    uint64_t inclusive_ns = 0;
    uint64_t self_ns = 0;
  };

  static constexpr std::array<const char*, static_cast<size_t>(Stage::Count)> StageNames = {
    "region_lookup", "image_discovery", "resolve_overlap", "image_refresh",
    "image_initialize", "image_copy", "cpu_alias_invalidate", "gpu_memory_invalidate",
    "garbage_collect", "register_image", "unregister_image", "resolve_texture",
    "rebind_images", "descriptor_commit", "texture_readback", "process_readbacks",
    "buffer_download", "buffer_upload", "buffer_synchronize", "buffer_acquire", "buffer_garbage_collect"
  };
  static constexpr std::array<const char*, static_cast<size_t>(Event::Count)> EventNames = {
    "rebind_reuse", "rebind_missing", "rebind_unregistered", "rebind_needed",
    "descriptor_push_writes", "descriptor_set_writes", "buffer_upload_staged_bytes",
    "buffer_upload_regions", "buffer_upload_batches", "buffer_readback_packed_bytes",
    "buffer_readback_payload_bytes", "buffer_readback_regions",
    "texture_readback_requested_bytes", "image_upload_requests", "image_upload_guest_bytes",
    "image_gc_checks", "image_gc_below_trigger", "image_gc_root_candidates",
    "image_gc_root_retire_requests", "image_gc_skipped_tiled",
    "image_gc_skipped_unpressured", "image_gc_skipped_readback", "resource_snapshots"
  };
  static constexpr std::array<const char*, static_cast<size_t>(Gauge::Count)> GaugeNames = {
    "image_slots_live", "image_slots_capacity", "image_slots_registered",
    "native_images_live", "image_views_live", "image_slots_peak_observed",
    "image_views_peak_observed"
  };

  class Scope final {
  public:
    Scope(TextureWorkloadAttribution& owner, Stage stage, bool enabled) noexcept
        : m_owner(enabled ? &owner : nullptr), m_stage(stage) {
      if (m_owner) {
        m_parent = Active();
        // Only nested scopes belonging to this accumulator are exclusive.
        // Find the matching ancestor even if another accumulator intervenes.
        m_accounting_parent = m_parent;
        while (m_accounting_parent && m_accounting_parent->m_owner != m_owner) {
          m_accounting_parent = m_accounting_parent->m_parent;
        }
        Active() = this;
        m_start = Clock::now();
      }
    }
    ~Scope() noexcept {
      if (!m_owner) return;
      const auto now = Clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_start).count();
      const auto inclusive = elapsed > 0 ? static_cast<uint64_t>(elapsed) : uint64_t{0};
      const auto self = inclusive >= m_children_ns ? inclusive - m_children_ns : uint64_t{0};
      m_owner->Record(m_stage, inclusive, self);
      Active() = m_parent;
      if (m_accounting_parent) {
        const auto space = std::numeric_limits<uint64_t>::max() - m_accounting_parent->m_children_ns;
        m_accounting_parent->m_children_ns += inclusive < space ? inclusive : space;
      }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

  private:
    using Clock = std::chrono::steady_clock;
    static Scope*& Active() noexcept {
      static thread_local Scope* current = nullptr;
      return current;
    }
    TextureWorkloadAttribution* m_owner = nullptr;
    Stage m_stage;
    Scope* m_parent = nullptr;
    Scope* m_accounting_parent = nullptr;
    Clock::time_point m_start{};
    uint64_t m_children_ns = 0;
  };

  void Record(Stage stage, uint64_t inclusive, uint64_t self) noexcept {
    if (self > inclusive) self = inclusive;
    auto& item = m_stages[static_cast<size_t>(stage)];
    item.calls.fetch_add(1, std::memory_order_relaxed);
    item.inclusive_ns.fetch_add(inclusive, std::memory_order_relaxed);
    item.self_ns.fetch_add(self, std::memory_order_relaxed);
  }

  void Count(Event event, uint64_t value = 1) noexcept {
    m_events[static_cast<size_t>(event)].fetch_add(value, std::memory_order_relaxed);
  }

  // Diagnostic snapshot: the caller serializes slot and view mutations.
  // Observed high-water is a lower bound because snapshots are sampled.
  void ObserveResourceSnapshot(uint64_t slots, uint64_t capacity, uint64_t registered,
                               uint64_t native_images, uint64_t views) noexcept {
    SetGauge(Gauge::ImageSlotsLive, slots);
    SetGauge(Gauge::ImageSlotsCapacity, capacity);
    SetGauge(Gauge::ImageSlotsRegistered, registered);
    SetGauge(Gauge::NativeImagesLive, native_images);
    SetGauge(Gauge::ImageViewsLive, views);
    ObserveMax(Gauge::ImageSlotsPeakObserved, slots);
    ObserveMax(Gauge::ImageViewsPeakObserved, views);
    Count(Event::ResourceSnapshots);
  }

  [[nodiscard]] uint64_t Get(Gauge gauge) const noexcept {
    return m_gauges[static_cast<size_t>(gauge)].load(std::memory_order_relaxed);
  }

  [[nodiscard]] Sample Get(Stage stage) const noexcept {
    const auto& item = m_stages[static_cast<size_t>(stage)];
    return {item.calls.load(std::memory_order_relaxed),
            item.inclusive_ns.load(std::memory_order_relaxed),
            item.self_ns.load(std::memory_order_relaxed)};
  }
  [[nodiscard]] uint64_t Get(Event event) const noexcept {
    return m_events[static_cast<size_t>(event)].load(std::memory_order_relaxed);
  }

  void WriteCsv(std::FILE* out) const noexcept {
    if (!out) return;
    std::fputs("kind,name,count,inclusive_ns,self_ns\n", out);
    for (size_t i = 0; i < StageNames.size(); ++i) {
      const auto s = Get(static_cast<Stage>(i));
      std::fprintf(out, "stage,%s,%llu,%llu,%llu\n", StageNames[i],
                   static_cast<unsigned long long>(s.calls),
                   static_cast<unsigned long long>(s.inclusive_ns),
                   static_cast<unsigned long long>(s.self_ns));
    }
    for (size_t i = 0; i < EventNames.size(); ++i) {
      std::fprintf(out, "event,%s,%llu,0,0\n", EventNames[i],
                   static_cast<unsigned long long>(Get(static_cast<Event>(i))));
    }
    for (size_t i = 0; i < GaugeNames.size(); ++i) {
      std::fprintf(out, "gauge,%s,%llu,0,0\n", GaugeNames[i],
                   static_cast<unsigned long long>(Get(static_cast<Gauge>(i))));
    }
  }

private:
  void SetGauge(Gauge g, uint64_t value) noexcept {
    m_gauges[static_cast<size_t>(g)].store(value, std::memory_order_relaxed);
  }
  void ObserveMax(Gauge g, uint64_t value) noexcept {
    auto& counter = m_gauges[static_cast<size_t>(g)];
    auto old = counter.load(std::memory_order_relaxed);
    while (old < value && !counter.compare_exchange_weak(
        old, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
  }
  struct Totals {
    std::atomic<uint64_t> calls{0}, inclusive_ns{0}, self_ns{0};
  };
  std::array<Totals, static_cast<size_t>(Stage::Count)> m_stages{};
  std::array<std::atomic<uint64_t>, static_cast<size_t>(Event::Count)> m_events{};
  std::array<std::atomic<uint64_t>, static_cast<size_t>(Gauge::Count)> m_gauges{};
};

} // namespace Libs::Graphics
#endif // KYTY_TEXTURE_WORKLOAD_ATTRIBUTION_H_
