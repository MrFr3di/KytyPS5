// S4-B5-D: minimal Windows x64 codegen probes. NOT a TextureCache disassembly.
// A C ABI/noinline boundary keeps four named machine-code functions visible.
// No timing or FPS assertions depend on the static assembly.
#include "graphics/host_gpu/renderer/cache/canonicalOwnerPageWalk.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "common/slotVector.h"
#include <cstddef>
#include <cstdint>

using Id=Common::SlotId;
using Owners=Libs::Graphics::InlinePageOwnerList<Id,16>;
using Table=Libs::Graphics::MultiLevelPageTable<Owners,20,44,14>;

#if defined(_MSC_VER)
#define KYTY_B5D_NOINLINE __declspec(noinline)
#else
#define KYTY_B5D_NOINLINE __attribute__((noinline))
#endif

extern "C" KYTY_B5D_NOINLINE uint64_t KytyB5DDirectFind(
    const Table& table, Table::PageRange r) {
  if(r.first>=r.last_exclusive||r.last_exclusive>Table::kPageCount)return 0;
  uint64_t checksum=0;
  for(size_t p=r.first;p<r.last_exclusive;++p){
    const auto* owners=table.Find(p);
    if(owners!=nullptr&&!owners->empty()){
      owners->ForEach([&](Id id){checksum+=(uint64_t(id.generation)<<32)|id.index;});
    }
  }
  return checksum;
}
extern "C" KYTY_B5D_NOINLINE uint64_t KytyB5DCanonicalLegacy(
    const Table& table, Table::PageRange r) {
  uint64_t checksum=0;
  Libs::Graphics::ForEachCanonicalOwnerPage<false>(table,r,[&](size_t,const Owners& owners){
    owners.ForEach([&](Id id){checksum+=(uint64_t(id.generation)<<32)|id.index;});
  });
  return checksum;
}
extern "C" KYTY_B5D_NOINLINE uint64_t KytyB5DPageOnly256(
    const Table& table, Table::PageRange r) {
  uint64_t checksum=0;
  Libs::Graphics::ForEachPageCountCanonicalOwnerPage<256>(
    table,r,[&](size_t,const Owners& owners){
      owners.ForEach([&](Id id){checksum+=(uint64_t(id.generation)<<32)|id.index;});
    });
  return checksum;
}
extern "C" KYTY_B5D_NOINLINE uint64_t KytyB5DManual256(
    const Table& table, Table::PageRange r) {
  uint64_t checksum=0;
  auto visit=[&](size_t,const Owners& owners){
    owners.ForEach([&](Id id){checksum+=(uint64_t(id.generation)<<32)|id.index;});
  };
  if(Libs::Graphics::ShouldSkipCanonicalBucketsByPageCount<256,Table>(r)){
    Libs::Graphics::ForEachCanonicalOwnerPage<true>(table,r,visit);
  }else{
    Libs::Graphics::ForEachCanonicalOwnerPage<false>(table,r,visit);
  }
  return checksum;
}
