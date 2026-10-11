#include "graphics/host_gpu/renderer/cache/textureWorkloadAttribution.h"
// Release CMake sets NDEBUG; research correctness tests must never become no-ops.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

using Stats = Libs::Graphics::TextureWorkloadAttribution;

int main() {
  Stats telemetry;
  assert(telemetry.Get(Stats::Stage::ImageDiscovery).calls == 0);
  {
    Stats::Scope outer(telemetry, Stats::Stage::ImageDiscovery, true);
    {
      Stats::Scope child(telemetry, Stats::Stage::RegionLookup, true);
      volatile uint64_t busy = 0;
      for (uint64_t i = 0; i < 10000; ++i) busy = busy + i;
      (void)busy;
    }
    { Stats::Scope disabled(telemetry, Stats::Stage::GarbageCollect, false); }
  }
  auto parent = telemetry.Get(Stats::Stage::ImageDiscovery);
  auto child = telemetry.Get(Stats::Stage::RegionLookup);
  assert(parent.calls == 1 && child.calls == 1);
  assert(parent.inclusive_ns >= parent.self_ns);
  assert(child.inclusive_ns >= child.self_ns);
  assert(telemetry.Get(Stats::Stage::GarbageCollect).calls == 0);
  assert(parent.inclusive_ns >= child.inclusive_ns);
  // A foreign recorder must not consume the parent's exclusive time.
  Stats foreign;
  {
    Stats::Scope outer(telemetry, Stats::Stage::ImageDiscovery, true);
    {
      Stats::Scope other(foreign, Stats::Stage::ImageDiscovery, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
      {
        Stats::Scope same_owner_descendant(telemetry, Stats::Stage::ImageCopy, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }
  const auto combined_outer = telemetry.Get(Stats::Stage::ImageDiscovery);
  const auto combined_child = telemetry.Get(Stats::Stage::ImageCopy);
  const auto foreign_child = foreign.Get(Stats::Stage::ImageDiscovery);
  // Across both outer calls: only same-recorder children can reduce self time.
  assert(combined_outer.calls == 2 && combined_child.calls == 1 && foreign_child.calls == 1);
  // Foreign-only work remains in self time; same-owner descendant is excluded.
  assert(combined_outer.self_ns + combined_child.inclusive_ns >= foreign_child.inclusive_ns);
  assert(combined_outer.inclusive_ns >= combined_outer.self_ns + combined_child.inclusive_ns);

  telemetry.Count(Stats::Event::RebindReuse);
  telemetry.Count(Stats::Event::DescriptorSetWrites, 7);
  telemetry.Count(Stats::Event::BufferUploadRegions, 3);
  telemetry.Count(Stats::Event::BufferUploadBatches);
  telemetry.Count(Stats::Event::BufferReadbackPayloadBytes, 123);
  telemetry.Count(Stats::Event::BufferReadbackPackedBytes, 128);
  telemetry.Count(Stats::Event::ImageUploadRequests);
  telemetry.Count(Stats::Event::ImageUploadGuestBytes, 8192);
  assert(telemetry.Get(Stats::Event::RebindReuse) == 1);
  assert(telemetry.Get(Stats::Event::DescriptorSetWrites) == 7);
  assert(telemetry.Get(Stats::Event::BufferUploadRegions) == 3);
  assert(telemetry.Get(Stats::Event::BufferUploadBatches) == 1);
  assert(telemetry.Get(Stats::Event::BufferReadbackPayloadBytes) == 123);
  assert(telemetry.Get(Stats::Event::BufferReadbackPackedBytes) == 128);
  assert(telemetry.Get(Stats::Event::ImageUploadRequests) == 1);
  assert(telemetry.Get(Stats::Event::ImageUploadGuestBytes) == 8192);
  // Snapshots are bounded and retain observed high-water, not a fake VRAM estimate.
  telemetry.ObserveResourceSnapshot(12, 20, 10, 9, 34);
  telemetry.ObserveResourceSnapshot(5, 20, 3, 4, 8);
  assert(telemetry.Get(Stats::Event::ResourceSnapshots) == 2);
  assert(telemetry.Get(Stats::Gauge::ImageSlotsLive) == 5);
  assert(telemetry.Get(Stats::Gauge::ImageSlotsCapacity) == 20);
  assert(telemetry.Get(Stats::Gauge::ImageSlotsRegistered) == 3);
  assert(telemetry.Get(Stats::Gauge::NativeImagesLive) == 4);
  assert(telemetry.Get(Stats::Gauge::ImageViewsLive) == 8);
  assert(telemetry.Get(Stats::Gauge::ImageSlotsPeakObserved) == 12);
  assert(telemetry.Get(Stats::Gauge::ImageViewsPeakObserved) == 34);
  telemetry.Count(Stats::Event::ImageGcChecks, 3);
  telemetry.Count(Stats::Event::ImageGcBelowTrigger, 2);
  telemetry.Count(Stats::Event::ImageGcRootCandidates, 4);
  telemetry.Count(Stats::Event::ImageGcRootRetireRequests);
  telemetry.Record(Stats::Stage::BufferGarbageCollect, 3, 5);
  auto clipped = telemetry.Get(Stats::Stage::BufferGarbageCollect);
  assert(clipped.calls == 1 && clipped.inclusive_ns == 3 && clipped.self_ns == 3);
  auto* file = std::tmpfile();
  assert(file);
  telemetry.WriteCsv(file);
  std::rewind(file);
  char line[256]{};
  assert(std::fgets(line, sizeof(line), file));
  assert(std::strcmp(line, "kind,name,count,inclusive_ns,self_ns\n") == 0);
  std::string csv(line);
  while (std::fgets(line, sizeof(line), file)) csv += line;
  std::fclose(file);
  assert(csv.find("event,rebind_reuse,1,0,0\n") != std::string::npos);
  assert(csv.find("event,descriptor_set_writes,7,0,0\n") != std::string::npos);
  assert(csv.find("event,buffer_upload_regions,3,0,0\n") != std::string::npos);
  assert(csv.find("event,buffer_readback_payload_bytes,123,0,0\n") != std::string::npos);
  assert(csv.find("event,image_upload_guest_bytes,8192,0,0\n") != std::string::npos);
  assert(csv.find("stage,buffer_garbage_collect,1,3,3\n") != std::string::npos);
  assert(csv.find("gauge,image_slots_live,5,0,0\n") != std::string::npos);
  assert(csv.find("gauge,image_views_peak_observed,34,0,0\n") != std::string::npos);
  assert(csv.find("event,image_gc_checks,3,0,0\n") != std::string::npos);
  std::puts("TextureWorkloadAttributionTests: PASS");
}
