#include "graphics/host_gpu/renderer/cache/textureWorkloadAttribution.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

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
  telemetry.Count(Stats::Event::RebindReuse);
  telemetry.Count(Stats::Event::DescriptorSetWrites, 7);
  assert(telemetry.Get(Stats::Event::RebindReuse) == 1);
  assert(telemetry.Get(Stats::Event::DescriptorSetWrites) == 7);
  telemetry.Record(Stats::Stage::ImageCopy, 3, 5);
  auto clipped = telemetry.Get(Stats::Stage::ImageCopy);
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
  assert(csv.find("stage,image_copy,1,3,3\n") != std::string::npos);
  std::puts("TextureWorkloadAttributionTests: PASS");
}
