// KYTY-INDEX-001 S4-B4b. Matching native SlotVector/SlotId A/B benchmark.
// SYNTHETIC workload only: never extrapolate a runner CPU ratio to GPU FPS.
// Mode 0 = production-shaped canonical MultiLevelPageTable only.
// Mode 1 = SAME canonical table + NativeLazyRangeSidecar and required fallback.
#include "graphics/host_gpu/renderer/cache/nativeLazyRangeSidecar.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {
struct alignas(std::max_align_t) Header { void* base; size_t size; bool tracked; };
bool track=false;
uint64_t live_bytes=0,peak_bytes=0,new_calls=0,free_calls=0,requested_bytes=0;
void* Alloc(size_t n,size_t alignment) {
  if(alignment<alignof(Header))alignment=alignof(Header);
  if((alignment&(alignment-1))!=0 ||
     n>std::numeric_limits<size_t>::max()-sizeof(Header)-alignment)
    throw std::bad_alloc{};
  const size_t len=(n?n:1)+sizeof(Header)+alignment;
  void* base=std::malloc(len);
  if(!base)throw std::bad_alloc{};
  auto ptr=reinterpret_cast<uintptr_t>(base)+sizeof(Header);
  ptr=(ptr+alignment-1)&~(uintptr_t(alignment)-1);
  auto* h=reinterpret_cast<Header*>(ptr)-1;
  *h={base,n,track};
  if(track){++new_calls;requested_bytes+=n;live_bytes+=n;peak_bytes=std::max(peak_bytes,live_bytes);}
  return reinterpret_cast<void*>(ptr);
}
void Free(void* p) noexcept {
  if(!p)return;
  const Header h=*(reinterpret_cast<Header*>(p)-1);
  if(h.tracked){if(live_bytes<h.size)std::abort();live_bytes-=h.size;++free_calls;}
  std::free(h.base);
}
}
void* operator new(size_t n){return Alloc(n,alignof(std::max_align_t));}
void* operator new[](size_t n){return Alloc(n,alignof(std::max_align_t));}
void operator delete(void* p) noexcept{Free(p);}
void operator delete[](void* p) noexcept{Free(p);}
void operator delete(void* p,size_t) noexcept{Free(p);}
void operator delete[](void* p,size_t) noexcept{Free(p);}
void* operator new(size_t n,std::align_val_t a){return Alloc(n,size_t(a));}
void* operator new[](size_t n,std::align_val_t a){return Alloc(n,size_t(a));}
void operator delete(void* p,std::align_val_t) noexcept{Free(p);}
void operator delete[](void* p,std::align_val_t) noexcept{Free(p);}
void operator delete(void* p,size_t,std::align_val_t) noexcept{Free(p);}
void operator delete[](void* p,size_t,std::align_val_t) noexcept{Free(p);}

namespace {
using Id=Common::SlotId;
using Side=Libs::Graphics::NativeLazyRangeSidecar;
using Owners=Side::Output;
using Table=Libs::Graphics::MultiLevelPageTable<Owners,20,44,14>;
using Clock=std::chrono::steady_clock;
constexpr uint64_t MiB=uint64_t{1}<<20;
constexpr uint64_t Base=uint64_t{3}<<30;
struct Image {
  uint64_t begin=0,bytes=0,effective_begin=0,effective_bytes=0;
  uint32_t query_epoch=0;
};
struct Spec{uint64_t begin,bytes;};
struct Query{uint64_t begin,bytes;bool rounded;};
struct Workload{std::vector<Spec> images;std::vector<Query> queries;};
void Check(bool cond,const char* why) {
  if(!cond){std::fprintf(stderr,"S4-B4b FAIL: %s\n",why);std::abort();}
}
uint64_t Random(uint64_t& x){x^=x<<13;x^=x>>7;x^=x<<17;return x;}
uint64_t Ns(Clock::time_point start){
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    Clock::now()-start).count());
}
bool Matches(const Image& im,uint64_t begin,uint64_t bytes,bool rounded) {
  if(bytes==0 || begin>=(uint64_t{1}<<44) ||
     bytes>(uint64_t{1}<<44)-begin)return false;
  const uint64_t end=begin+bytes, im_end=im.effective_begin+im.effective_bytes;
  if(!rounded)return im.effective_begin<end&&begin<im_end;
  return (im.effective_begin>>12)<=((end-1)>>12) &&
         (begin>>12)<=((im_end-1)>>12);
}
Workload Generate(int scenario){
  Workload w;
  const unsigned count=scenario==0?64u:(scenario==1?128u:96u);
  w.images.reserve(count);
  for(unsigned i=0;i<count;++i){
    uint64_t begin,bytes;
    if(scenario==0){begin=Base+uint64_t(i)*4096;bytes=16*MiB;}
    else if(scenario==1){begin=Base+uint64_t(i)*64*MiB+uint64_t(i%3)*4096;bytes=4096;}
    else{begin=Base+uint64_t(i%24)*MiB+uint64_t(i)*8192;bytes=(2+uint64_t(i%6))*MiB;}
    w.images.push_back({begin,bytes});
  }
  uint64_t seed=0x63b8410adc00ba5full+uint64_t(scenario)*1093;
  w.queries.reserve(320);
  for(unsigned q=0;q<320;++q){
    const unsigned owner=unsigned(Random(seed)%count);
    const auto s=w.images[owner];
    const uint64_t delta=Random(seed)%4096;
    const uint64_t address=s.begin+delta;
    const uint64_t bytes=(q%5==0)?4096:((q%7==0)?32*MiB:16*MiB);
    w.queries.push_back({address,bytes,(q%3)==0});
  }
  return w;
}
struct Index {
  explicit Index(bool h1):sidecar([]{
    Side::Config c;
    c.min_query_pages=4;
    c.build_after_large_queries=4;
    c.block_size=16;
    c.max_snapshot_payload_bytes=1024*1024;
    return c;
  }()),use_sidecar(h1){}
  Common::SlotVector<Image> images;
  Table canonical;
  Side sidecar;
  std::vector<Id> active;
  bool use_sidecar=false;
  uint32_t epoch=0;
  size_t answered=0,fallback=0,inspected=0,builds=0;
  Id Register(Spec s){
    Check(Side::Valid(s.begin,s.bytes),"register bounds");
    const Id id=images.insert(Image{s.begin,s.bytes,s.begin,s.bytes,0});
    Table::PageRange range{};
    Check(Table::TryGetPageRange(s.begin,s.bytes,range),"page range");
    for(size_t p=range.first;p<range.last_exclusive;++p)
      canonical[p].push_back(id);
    if(use_sidecar)Check(sidecar.Register(id,s.begin,s.bytes),"native register");
    active.push_back(id);
    return id;
  }
  void Unregister(Id id){
    const auto* image=images.try_get(id);
    Check(image!=nullptr,"id live at unregister");
    Table::PageRange range{};
    Check(Table::TryGetPageRange(image->begin,image->bytes,range),"remove range");
    for(size_t p=range.first;p<range.last_exclusive;++p){
      auto* owners=canonical.Find(p);
      Check(owners&&owners->Erase(id),"canonical delete exact native key");
    }
    if(use_sidecar)Check(sidecar.Unregister(id),"sidecar delete exact native key");
    images.erase(id);
    const auto found=std::find(active.begin(),active.end(),id);
    Check(found!=active.end(),"active set");
    active.erase(found);
  }
  void Mutate(Id id){
    auto* im=images.try_get(id);Check(im!=nullptr,"mutable image live");
    im->effective_begin=im->begin+128*1024;
    im->effective_bytes=std::min<uint64_t>(im->bytes,64*1024);
    // Registration footprint unchanged; both paths must read current value.
  }
  Owners Canonical(const Query& q){
    Table::PageRange range{};
    Check(Table::TryGetPageRange(q.begin,q.bytes,range),"valid query");
    if(++epoch==0){
      images.ForEach([](Id,Image& im){im.query_epoch=0;});
      epoch=1;
    }
    Owners result;
    for(size_t p=range.first;p<range.last_exclusive;++p){
      const auto* owners=canonical.Find(p);
      if(!owners)continue;
      owners->ForEach([&](Id id){
        auto* image=images.try_get(id);
        if(image==nullptr||image->query_epoch==epoch)return;
        image->query_epoch=epoch;
        if(Matches(*image,q.begin,q.bytes,q.rounded))result.push_back(id);
      });
    }
    return result;
  }
  Owners Find(const Query& q){
    if(!use_sidecar)return Canonical(q);
    Owners result;
    Side::Stats stats;
    const bool hit=sidecar.TryFind(q.begin,q.bytes,q.rounded,
      [this](Id id,uint64_t begin,uint64_t bytes,bool rounded){
        const auto* im=images.try_get(id);
        return im && Matches(*im,begin,bytes,rounded);
      },result,&stats);
    inspected+=stats.inspected;
    builds+=size_t(stats.built);
    if(hit){++answered;return result;}
    ++fallback;
    return Canonical(q);
  }
  Owners IndependentOracle(const Query& q) const{
    Table::PageRange range{};
    Check(Table::TryGetPageRange(q.begin,q.bytes,range),"oracle pages");
    Owners result;
    // Intentionally independent from PageTable and H1 snapshot:
    // page-major scan and active registration order.
    for(size_t page=range.first;page<range.last_exclusive;++page){
      for(Id id:active){
        const auto* im=images.try_get(id);
        Check(im!=nullptr,"oracle native ID alive");
        const size_t lo=size_t(im->begin>>20);
        const size_t hi=size_t(((im->begin+im->bytes-1)>>20)+1);
        if(!(lo<=page&&page<hi)||result.Contains(id))continue;
        if(Matches(*im,q.begin,q.bytes,q.rounded))result.push_back(id);
      }
    }
    return result;
  }
};
uint64_t Hash(uint64_t hash,const Owners& ids){
  for(const Id id:ids){
    hash^=(uint64_t(id.generation)<<32)|id.index;
    hash*=1099511628211ull;
  }
  hash^=ids.size();hash*=1099511628211ull;
  return hash;
}
Query WarmQuery(const Workload& w,unsigned n){
  return {w.images[n%w.images.size()].begin,32*MiB,(n%2)==0};
}
void Churn(Index& idx,const Workload& w){
  const size_t count=std::min<size_t>(16,w.images.size()/4);
  for(size_t i=0;i<count;++i){
    const Id old=idx.active[i*2];
    const Spec spec{idx.images[old].begin,idx.images[old].bytes};
    idx.Unregister(old);
    const Id now=idx.Register(spec);
    Check(now.index==old.index&&now.generation!=old.generation,
          "SlotVector generation reused with a different generation");
    Check(idx.images.try_get(old)==nullptr,"stale native generation rejected");
  }
  for(size_t i=0;i<4;++i)idx.Mutate(idx.active[i]);
}
bool Same(const Owners& a,const Owners& b) {
  if(a.size()!=b.size())return false;
  for(size_t i=0;i<a.size();++i)if(a[i]!=b[i])return false;
  return true;
}
void Preflight(const Workload& w){
  Index canonical(false),h1(true);
  for(const auto s:w.images)
    Check(canonical.Register(s)==h1.Register(s),"matched native registrations");
  const auto query=[&](const Query& q){
    const auto expected=canonical.IndependentOracle(q);
    const auto a=canonical.Find(q),b=h1.Find(q);
    Check(Same(a,expected),
          "canonical matches independent page-major/native-key oracle");
    Check(Same(b,expected),
          "sidecar+fallback same ordered native IDs");
  };
  for(unsigned i=0;i<4;++i)query(WarmQuery(w,i));
  for(const Query& q:w.queries)query(q);
  const Query small{w.images[0].begin,4096,true};
  Check(!canonical.Find(small).empty(),"small query needs canonical fallback");
  Owners untouched;
  Check(!h1.sidecar.TryFind(small.begin,small.bytes,small.rounded,
    [](Id,uint64_t,uint64_t,bool){return true;},untouched),
    "no sidecar answer on one-page lookup");
  Check(untouched.empty(),"denied sidecar cannot mutate output");
  Churn(canonical,w);Churn(h1,w);
  for(unsigned i=0;i<4;++i)query(WarmQuery(w,i+4));
  for(const Query& q:w.queries)query(q);
  Check(h1.answered>0&&h1.fallback>0,"both read-only H1 and canonical fallback exercised");
  Check(h1.sidecar.Validate(),"valid native snapshot across churn");
}
struct Phase{uint64_t ns=0,new_calls=0;};
template<typename Fn> Phase Time(Fn&& run){
  const uint64_t before=new_calls;
  const auto start=Clock::now();
  run();
  return {Ns(start),new_calls-before};
}
struct Row{
  Phase registration,warm,query,churn,rewarm,requery;
  uint64_t checksum=1469598103934665603ull;
  uint64_t live_after_register=0,live_after_warm=0,live_after_queries=0;
  uint64_t peak=0,live_after_churn=0,requested=0,allocs=0,frees=0;
  size_t buckets=0,sidecar_payload=0,builds=0,answered=0,fallback=0,inspected=0;
};
Row Replay(const Workload& w,bool with_sidecar) {
  Check(!track&&live_bytes==0,"clean process benchmark state");
  track=true;
  Row row;
  {
    Index index(with_sidecar);
    row.registration=Time([&]{
      for(const Spec s:w.images)(void)index.Register(s);
    });
    row.live_after_register=live_bytes;
    row.warm=Time([&]{
      for(unsigned i=0;i<4;++i)row.checksum=Hash(row.checksum,index.Find(WarmQuery(w,i)));
    });
    row.live_after_warm=live_bytes;
    row.query=Time([&]{
      for(const Query& q:w.queries)row.checksum=Hash(row.checksum,index.Find(q));
    });
    row.live_after_queries=live_bytes;
    row.churn=Time([&]{Churn(index,w);});
    row.live_after_churn=live_bytes;
    row.rewarm=Time([&]{
      for(unsigned i=0;i<4;++i)row.checksum=Hash(row.checksum,index.Find(WarmQuery(w,i+4)));
    });
    row.requery=Time([&]{
      for(const Query& q:w.queries)row.checksum=Hash(row.checksum,index.Find(q));
    });
    Check(index.sidecar.Validate(),"snapshot correct");
    row.buckets=index.canonical.AllocatedBucketCount();
    row.sidecar_payload=with_sidecar?index.sidecar.NominalPayloadBytes():0;
    row.builds=index.builds;
    row.answered=index.answered;row.fallback=index.fallback;
    row.inspected=index.inspected;
  }
  track=false;
  Check(live_bytes==0,"all tracked heap objects freed after benchmark");
  row.peak=peak_bytes;row.requested=requested_bytes;
  row.allocs=new_calls;row.frees=free_calls;
  return row;
}
} // namespace
int main(int argc,char** argv){
  if(argc!=4){std::fputs("usage: native_matched_bench scenario(0..2) variant(0..1) repetition(0..2)\n",stderr);return 2;}
  const int scenario=std::atoi(argv[1]),variant=std::atoi(argv[2]),repetition=std::atoi(argv[3]);
  if(scenario<0||scenario>2||variant<0||variant>1||repetition<0||repetition>2)return 2;
  const Workload w=Generate(scenario);
  Preflight(w);  // Verify EVERY cold/warm/churn/query result before timing.
  const Row r=Replay(w,variant!=0);
  const auto name=scenario==0?"dense_overlap":(scenario==1?"sparse_disjoint":"generation_churn");
  std::printf("%s,%s,%d,%zu,%zu,%llu,%llu,%llu,%llu,%llu,%llu,"
    "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
    "%llu,%llu,%llu,%llu,%llu,%zu,%zu,%zu,%zu,%zu,%d\n",
    name,variant?"canonical_plus_native_lazy":"canonical_only",repetition,
    w.images.size(),w.queries.size()*2+8,
    static_cast<unsigned long long>(r.registration.ns),
    static_cast<unsigned long long>(r.warm.ns),
    static_cast<unsigned long long>(r.query.ns),
    static_cast<unsigned long long>(r.churn.ns),
    static_cast<unsigned long long>(r.rewarm.ns),
    static_cast<unsigned long long>(r.requery.ns),
    static_cast<unsigned long long>(r.registration.new_calls),
    static_cast<unsigned long long>(r.warm.new_calls),
    static_cast<unsigned long long>(r.query.new_calls),
    static_cast<unsigned long long>(r.churn.new_calls),
    static_cast<unsigned long long>(r.rewarm.new_calls),
    static_cast<unsigned long long>(r.requery.new_calls),
    static_cast<unsigned long long>(r.live_after_register),
    static_cast<unsigned long long>(r.live_after_warm),
    static_cast<unsigned long long>(r.live_after_queries),
    static_cast<unsigned long long>(r.live_after_churn),
    static_cast<unsigned long long>(r.peak),
    static_cast<unsigned long long>(r.requested),
    static_cast<unsigned long long>(r.allocs),
    static_cast<unsigned long long>(r.frees),
    static_cast<unsigned long long>(r.checksum),
    r.buckets,r.sidecar_payload,r.builds,r.answered,r.fallback,int(variant!=0));
}
