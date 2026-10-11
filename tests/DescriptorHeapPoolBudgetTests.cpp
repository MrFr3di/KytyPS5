// G3-D4: per-pool epoch high-water, source-referenced capacities.
// No Vulkan dependency; emitted CSV is SYNTHETIC, not a game trace.
#include "graphics/host_gpu/renderer/pipeline/descriptorHeapTelemetry.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using Libs::Graphics::DescriptorHeapTelemetry;
static void Require(bool pass,const char* why){
  if(!pass){std::fprintf(stderr,"G3-D4 FAIL %s\n",why);std::abort();}
}
static void Allocate(DescriptorHeapTelemetry& s,uint32_t sets,
                     const std::array<uint64_t,4>& weight){
  s.AllocateAttempt();
  s.Allocated(sets);
  s.AllocatedTyped(sets,weight,true);
}
static void Issue(DescriptorHeapTelemetry& s,const std::array<uint64_t,4>& weight,
                  bool hit=false){
  if(hit)s.CacheHit();else s.CacheMiss();
  s.Issued();s.IssuedTyped(weight,true);
}
static DescriptorHeapTelemetry ValidFixture(){
  DescriptorHeapTelemetry s;
  const std::array<uint64_t,4> one{1,1,1,1};
  s.PoolCreated();
  Allocate(s,800,one);Issue(s,one);
  s.PoolRetired(1);
  s.DiscardUnused(799);s.DiscardTyped(799,{799,799,799,799},true);
  // Change to a freshly created pool WITHOUT conflating cumulative costs.
  s.PoolCreated();
  Allocate(s,700,one);Issue(s,one);
  Require(s.Valid()&&s.TypedKnown()&&s.PoolBudgetKnown()&&s.PoolBudgetFeasible(),
          "two epochs individually fit");
  Require(s.Snapshot().allocated_sets==1500 && s.PeakPoolSets()==800,
          "lifetime >1024 but max physical pool consumption <=1024");
  Require(s.PeakPoolTypes()==std::array<uint64_t,4>{800,800,800,800},
          "per-resource high-water derives from success only");
  return s;
}
int main(int argc,char** argv){
  const auto fixture=ValidFixture();
  if(argc==2&&std::strcmp(argv[1],"--csv")==0){
    fixture.WriteCsv(stdout);
    return 0;
  }
  Require(argc==1,"usage no arguments or --csv");
  // One pool with more than 1024 synthetic allocated sets is a counterexample.
  {
    DescriptorHeapTelemetry s;
    s.PoolCreated();Allocate(s,1025,{0,0,0,0});
    Require(s.Valid()&&s.TypedKnown()&&s.PeakPoolSets()==1025 &&
            !s.PoolBudgetFeasible(),"maxSets breach rejected");
  }
  // Only sampler budget exceeds 1024; set count remains below 1024.
  {
    DescriptorHeapTelemetry s;
    s.PoolCreated();Allocate(s,200,{0,0,0,6});
    Require(s.Valid()&&s.TypedKnown()&&s.PeakPoolSets()==200 &&
            s.PeakPoolTypes()[3]==1200&&!s.PoolBudgetFeasible(),
            "sampler over-cap correctly detected");
  }
  // Failure to allocate MUST NOT charge any descriptors to the pool.
  {
    DescriptorHeapTelemetry s;
    s.PoolCreated();s.AllocateAttempt();s.AllocateRejected(false);
    Require(s.PeakPoolSets()==0&&s.PeakPoolTypes()==std::array<uint64_t,4>{},
            "rejected driver allocation does not consume budget");
  }
  // Reset starts a new epoch after success but preserves lifetime totals.
  {
    DescriptorHeapTelemetry s;
    s.PoolCreated();Allocate(s,512,{1,0,0,0});
    s.PoolRetired(1);
    s.DiscardUnused(512);s.DiscardTyped(512,{512,0,0,0},true);
    s.PoolReset();
    Allocate(s,64,{1,0,0,0});
    Require(s.Valid()&&s.PoolBudgetFeasible()&&s.PeakPoolSets()==512 &&
            s.Snapshot().allocated_sets==576 && s.Snapshot().reset_pools==1,
            "pool reset only resets per-epoch counters");
  }
  // Unknown descriptor kind MUST NOT be silently treated as zero occupancy.
  {
    DescriptorHeapTelemetry s;
    s.PoolCreated();s.AllocateAttempt();s.Allocated(3);
    s.AllocatedTyped(3,{1,0,0,0},false);
    Require(!s.PoolBudgetKnown()&&!s.PoolBudgetFeasible() &&
            s.PeakPoolTypes()==std::array<uint64_t,4>{},
            "unknown type fails closed");
  }
  std::puts("summary,epochs,2,lifetime_sets,1500,peak_pool_sets,800,"
            "resource_types,4,counterexamples,4,source,SYNTHETIC,"
            "decision,G3_D4_POOL_BUDGET_PASS");
  return 0;
}
