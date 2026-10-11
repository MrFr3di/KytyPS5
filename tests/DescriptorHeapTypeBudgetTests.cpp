// G3-D3: exact weighted budgets using the same model as VkLayout creation.
#include "graphics/host_gpu/renderer/pipeline/descriptorHeapTypeBudget.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorHeapTelemetry.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using Libs::Graphics::DescriptorBudgetKind;
using Libs::Graphics::DescriptorLayoutBudget;
using Libs::Graphics::DescriptorHeapTelemetry;
void Check(bool yes,const char* msg) {
  if(!yes){std::fprintf(stderr,"G3-D3 FAIL: %s\n",msg);std::abort();}
}
int main() {
  DescriptorLayoutBudget layout;
  layout.Add(DescriptorBudgetKind::StorageBuffer,2);
  layout.Add(DescriptorBudgetKind::StorageBuffer,3);
  layout.Add(DescriptorBudgetKind::SampledImage,4);
  layout.Add(DescriptorBudgetKind::StorageImage,1);
  layout.Add(DescriptorBudgetKind::Sampler,6);
  const std::array<uint64_t,4> weights{5,4,1,6};
  Check(layout.Known()&&layout.Counts()==weights,"native type bindings");
  DescriptorHeapTelemetry t;
  t.PoolCreated();t.AllocateAttempt();t.Allocated(16);
  t.AllocatedTyped(16,weights,true);
  for(unsigned i=0;i<7;++i) {
    if(i==0)t.CacheMiss();else t.CacheHit();
    t.Issued();t.IssuedTyped(weights,true);
    Check(t.Valid(),"per-issue budget");
  }
  t.PoolRetired(1);
  const std::array<uint64_t,4> discard{45,36,9,54};
  t.DiscardUnused(9);t.DiscardTyped(9,discard,true);
  Check(t.Valid()&&t.TypedKnown(),"all four types complete");
  const auto stats=t.TypeSnapshot();
  const std::array<uint64_t,4> total{80,64,16,96};
  const std::array<uint64_t,4> issued{35,28,7,42};
  Check(stats.allocated==total&&stats.issued==issued&&
        stats.discarded==discard,"weighted exact ledger");
  for(size_t i=0;i<4;++i)
    Check(stats.allocated[i]==stats.issued[i]+stats.discarded[i]+stats.cached[i],
          "type conservation");
  auto fp=std::tmpfile();Check(fp!=nullptr,"tmpfile");
  t.WriteCsv(fp);std::rewind(fp);char csv[4096]{};
  const auto n=std::fread(csv,1,sizeof(csv)-1,fp);std::fclose(fp);
  Check(n>100&&std::strstr(csv,"type_counts_known,1\n"),"fully known");
  Check(std::strstr(csv,"allocated_storage_buffer,80\n")!=nullptr,"type count CSV");
  Check(std::strstr(csv,"discarded_sampler,54\n")!=nullptr,"discarded type CSV");
  auto missing=t;missing.Allocated(1);
  Check(!missing.Valid()&&!missing.TypedKnown(),"missing typed allocation");
  auto overdraw=t;overdraw.IssuedTyped({999,0,0,0},true);
  Check(!overdraw.Valid(),"typed overdraft");
  auto duplicate=t;duplicate.DiscardTyped(1,{0,0,0,0},true);
  Check(!duplicate.Valid(),"duplicate type discard");
  DescriptorLayoutBudget unknown;
  unknown.Add(DescriptorBudgetKind::StorageBuffer,1);
  unknown.Add(DescriptorBudgetKind::Unsupported,27);
  Check(!unknown.Known(),"unsupported fails closed");
  DescriptorHeapTelemetry u;
  u.PoolCreated();u.AllocateAttempt();u.Allocated(3);
  u.AllocatedTyped(3,unknown.Counts(),unknown.Known());
  u.CacheMiss();u.Issued();u.IssuedTyped(unknown.Counts(),unknown.Known());
  u.DiscardUnused(2);u.DiscardTyped(2,{0,0,0,0},false);
  Check(u.Valid()&&!u.TypedKnown(),"unknown must not claim exact");
  fp=std::tmpfile();Check(fp!=nullptr,"tmpfile unknown");
  u.WriteCsv(fp);std::rewind(fp);char csv_unknown[4096]{};
  const auto n_unknown=std::fread(csv_unknown,1,sizeof(csv_unknown)-1,fp);
  std::fclose(fp);
  Check(n_unknown>100,"unknown CSV nonempty");
  Check(std::strstr(csv_unknown,"type_counts_known,0\n")!=nullptr,
        "unknown reported");
  Check(std::strstr(csv_unknown,"allocated_storage_buffer,0\n")!=nullptr,
        "no partial metrics");
  std::puts("summary,type_buckets,4,negative_mutants,3,"
            "source,VkDescriptorSetLayoutBinding,decision,G3_D3_TYPE_BUDGET_PASS");
}
