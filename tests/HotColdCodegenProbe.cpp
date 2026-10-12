// KYTY-INDEX-001 S4-B5-E optimized x64 COFF MINIMAL codegen probes.
// Not production TextureCache; checksum callback omits epoch/image::Overlaps.
#include "graphics/host_gpu/renderer/cache/canonicalOwnerPageWalk.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "common/slotVector.h"
#include <cstddef>
#include <cstdint>
using Id=Common::SlotId;
using Owners=Libs::Graphics::InlinePageOwnerList<Id,16>;
using Table=Libs::Graphics::MultiLevelPageTable<Owners,20,44,14>;
#if defined(_MSC_VER)
#define KYTY_E_NOINLINE __declspec(noinline)
#else
#define KYTY_E_NOINLINE __attribute__((noinline))
#endif
extern "C" KYTY_E_NOINLINE uint64_t KytyB5ELegacy(
    const Table& table,Table::PageRange r){
  uint64_t sum=0;
  Libs::Graphics::ForEachCanonicalOwnerPage<false>(table,r,[&](size_t,const Owners& v){
    v.ForEach([&](Id id){sum+=(uint64_t(id.generation)<<32)|id.index;});
  });
  return sum;
}
extern "C" KYTY_E_NOINLINE uint64_t KytyB5EInline256(
    const Table& table,Table::PageRange r){
  uint64_t sum=0;
  Libs::Graphics::ForEachPageCountCanonicalOwnerPage<256>(table,r,[&](size_t,const Owners& v){
    v.ForEach([&](Id id){sum+=(uint64_t(id.generation)<<32)|id.index;});
  });
  return sum;
}
// A stable named NOINLINE cold body, taking checksum state as an explicit
// pointer. No second data structure or allocation and no hidden thread state.
extern "C" KYTY_E_NOINLINE void KytyB5EColdSkip(
    const Table& table,Table::PageRange r,uint64_t* checksum){
  Libs::Graphics::ForEachCanonicalOwnerPage<true>(table,r,[&](size_t,const Owners& v){
    v.ForEach([&](Id id){*checksum+=(uint64_t(id.generation)<<32)|id.index;});
  });
}
extern "C" KYTY_E_NOINLINE uint64_t KytyB5EHotCold256(
    const Table& table,Table::PageRange r){
  uint64_t sum=0;
  if(Libs::Graphics::ShouldSkipCanonicalBucketsByPageCount<256,Table>(r)){
    KytyB5EColdSkip(table,r,&sum);
  }else{
    Libs::Graphics::ForEachCanonicalOwnerPage<false>(table,r,[&](size_t,const Owners& v){
      v.ForEach([&](Id id){sum+=(uint64_t(id.generation)<<32)|id.index;});
    });
  }
  return sum;
}
extern "C" KYTY_E_NOINLINE uint64_t KytyB5EFullDispatcher256(
    const Table& table,Table::PageRange r){
  uint64_t sum=0;
  auto visit=[&](size_t,const Owners& v){
    v.ForEach([&](Id id){sum+=(uint64_t(id.generation)<<32)|id.index;});
  };
  if(Libs::Graphics::ShouldSkipCanonicalBucketsByPageCount<256,Table>(r))
    Libs::Graphics::ForEachCanonicalOwnerPage<true>(table,r,visit);
  else
    Libs::Graphics::ForEachCanonicalOwnerPage<false>(table,r,visit);
  return sum;
}
