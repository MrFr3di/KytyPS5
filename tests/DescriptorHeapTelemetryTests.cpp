// G3-D2: real header-only accumulator; no Vulkan, scheduler, GPU or PC needed.
#include "graphics/host_gpu/renderer/pipeline/descriptorHeapTelemetry.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
using Libs::Graphics::DescriptorHeapTelemetry;
void Require(bool x) { if (!x) std::abort(); }
int main() {
  DescriptorHeapTelemetry t;
  t.PoolCreated();
  t.AllocateAttempt();
  t.Allocated(32);
  for (unsigned i=0;i<12;++i) {
    if (i == 0) t.CacheMiss();
    else t.CacheHit();
    t.Issued();
    Require(t.Valid());
  }
  Require(t.CachedUnusedSets()==20);
  t.AllocateAttempt();
  t.AllocateRejected(false);
  t.AllocateAttempt();
  t.AllocateRejected(true);
  t.PoolRetired(1);
  t.DiscardUnused(20);
  Require(t.CachedUnusedSets()==0 && t.Valid());
  t.PoolCreated();
  t.AllocateAttempt();
  t.Allocated(8);
  t.CacheMiss(); t.Issued();
  t.PoolRetired(2);
  t.DiscardUnused(7);
  t.PoolReset();
  Require(t.CachedUnusedSets()==0 && t.Valid());
  const auto s=t.Snapshot();
  Require(s.allocated_sets==40 && s.issued_sets==13);
  Require(s.discarded_unused_sets==27 && s.cache_hits==11);
  Require(s.allocation_attempts==4 && s.rejected_out_of_pool==1 &&
          s.rejected_fragmented==1 && s.peak_pending_pools==2);
  Require(s.created_pools==2 && s.reset_pools==1 && s.retired_pools==2);
  // Deliberately wrong state transitions are independently rejected.
  {
    auto m=t; m.Issued(); Require(!m.Valid()); // missing cache hit or miss
  }
  {
    auto m=t; m.DiscardUnused(1); Require(!m.Valid()); // extra discard
  }
  {
    auto m=t; m.AllocateRejected(false);
    m.AllocateRejected(true);
    m.AllocateRejected(false);
    Require(!m.Valid()); // rejected calls cannot outnumber attempts
  }
  {
    auto m=t; m.PoolReset(); m.PoolReset(); Require(!m.Valid());
  }
  // End-of-run CSV follows schema and never claims descriptor type counts.
  std::FILE* fp=std::tmpfile();
  Require(fp!=nullptr);
  t.WriteCsv(fp);
  std::rewind(fp);
  char csv[4096]{};
  const size_t n=std::fread(csv,1,sizeof(csv)-1,fp);
  std::fclose(fp);
  Require(n>100 && n<sizeof(csv)-1);
  Require(std::strstr(csv,"metric,value\n")!=nullptr);
  Require(std::strstr(csv,"allocated_sets,40\n")!=nullptr);
  Require(std::strstr(csv,"issued_sets,13\n")!=nullptr);
  Require(std::strstr(csv,"discarded_unused_sets,27\n")!=nullptr);
  Require(std::strstr(csv,"type_counts_known,0\n")!=nullptr);
  Require(std::strstr(csv,"gpu_timing_known,0\n")!=nullptr);
  Require(std::strstr(csv,"invariant_pass,1\n")!=nullptr);
  std::puts("summary,checks,header_accumulator,negative_mutants,4,"
            "type_counts_known,0,decision,G3_D2_TELEMETRY_PASS");
}
