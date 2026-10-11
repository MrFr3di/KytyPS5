// KYTY-INDEX-001 S4-B5-A — production-shaped native canonical bucket walk.
// Tests same helper used by TextureCache; the guest Image::Overlaps is replaced
// with a CPU-only equivalent effective-range predicate. NO GPU or FPS claims.
#include "graphics/host_gpu/renderer/cache/canonicalOwnerPageWalk.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "common/slotVector.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>
#include <type_traits>

namespace {
using Id=Common::SlotId;
using Owners=Libs::Graphics::InlinePageOwnerList<Id,16>;
using Pages=Libs::Graphics::MultiLevelPageTable<Owners,20,44,14>;
using Clock=std::chrono::steady_clock;
constexpr uint64_t MiB=1ull<<20,GiB=1ull<<30,Limit=1ull<<44;
constexpr uint64_t Base=uint64_t{2}<<30;
uint64_t alloc_requests=0;
bool count_allocations=false;
void CountNew(){if(count_allocations)++alloc_requests;}
void Check(bool yes,const char* msg){
  if(!yes){std::fprintf(stderr,"S4-B5-A FAIL %s\n",msg);std::abort();}
}
uint64_t Ns(Clock::time_point start){
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now()-start).count());
}
struct Image {
  uint64_t begin=0,bytes=0,effective_begin=0,effective_bytes=0;
  mutable uint32_t query_epoch=0;
};
struct Spec{uint64_t begin,bytes;};
struct Query{uint64_t begin,bytes;bool rounded;};
uint64_t Random(uint64_t& s){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
bool Overlaps(const Image& im,const Query& q) {
  const uint64_t e=q.begin+q.bytes,im_e=im.effective_begin+im.effective_bytes;
  if(!q.rounded)return im.effective_begin<e&&q.begin<im_e;
  return (im.effective_begin>>12)<=((e-1)>>12)&&
         (q.begin>>12)<=((im_e-1)>>12);
}
struct Workload{
  std::vector<Spec> specs;
  std::vector<Query> queries;
};
Workload Make(int scenario){
  Workload w;
  const int n=scenario==0?64:scenario==1?32:scenario==2?72:8;
  w.specs.reserve(n);
  for(int i=0;i<n;++i){
    if(scenario==0)w.specs.push_back({Base+uint64_t(i)*4096,(4+uint64_t(i%6))*MiB});
    else if(scenario==1)w.specs.push_back({Base+uint64_t(i)*4*GiB+4096,4096});
    else if(scenario==2)w.specs.push_back({Base+uint64_t(i%24)*MiB+uint64_t(i)*4096,8*MiB});
    else{
      const uint64_t range=i<4?(Base+uint64_t(1022+i)*MiB):
              (Limit-(16-uint64_t(i-4))*MiB);
      w.specs.push_back({range,MiB});
    }
  }
  uint64_t seed=0x71a8ebc135c03ull+uint64_t(scenario)*19;
  for(int i=0;i<96;++i){
    auto owner=static_cast<size_t>(Random(seed)%w.specs.size());
    auto spec=w.specs[owner];
    uint64_t offset=Random(seed)%4096;
    uint64_t address=spec.begin+offset;
    uint64_t size=(i%4==0?4096:(scenario==1?8*GiB:scenario==2?2*GiB:64*MiB));
    if(size>Limit-address)size=Limit-address;
    w.queries.push_back({address,size,(i%3==0)});
  }
  return w;
}
struct Model{
  Common::SlotVector<Image> images;
  Pages pages;
  std::vector<Id> active;
  uint32_t epoch=0;
  size_t emitted_pages=0,inspected_owners=0;
  Id Register(Spec spec){
    Pages::PageRange range{};
    Check(Pages::TryGetPageRange(spec.begin,spec.bytes,range),"register bounds");
    const Id id=images.insert(Image{spec.begin,spec.bytes,spec.begin,spec.bytes,0});
    for(size_t page=range.first;page<range.last_exclusive;++page)
      pages[page].push_back(id);
    active.push_back(id);
    return id;
  }
  void Unregister(Id id){
    const auto* image=images.try_get(id);Check(image!=nullptr,"unregister live");
    Pages::PageRange range{};
    Check(Pages::TryGetPageRange(image->begin,image->bytes,range),"unregister bounds");
    for(size_t page=range.first;page<range.last_exclusive;++page){
      auto* owners=pages.Find(page);
      Check(owners!=nullptr&&owners->Erase(id),"erase native key");
    }
    images.erase(id);
    auto it=std::find(active.begin(),active.end(),id);
    Check(it!=active.end(),"active id");
    active.erase(it);
  }
  void Churn(){
    const auto churn_count=std::min<size_t>(12,active.size()/2);
    for(size_t k=0;k<churn_count;++k){
      Id old=active[k*2];
      const auto* im=images.try_get(old);
      const Spec spec{im->begin,im->bytes};
      Unregister(old);
      Id newer=Register(spec);
      Check(newer.index==old.index&&newer.generation!=old.generation,
            "generation reuse");
      Check(images.try_get(old)==nullptr,"stale native ID");
    }
    for(size_t i=0;i<4;++i){
      Image* im=images.try_get(active[i]);
      Check(im!=nullptr,"effective mutation live");
      im->effective_begin+=256*1024;
      im->effective_bytes=std::min<uint64_t>(im->effective_bytes,4096);
    }
  }
  template<bool Skip> Owners QueryPage(const Query& q) {
    Pages::PageRange r{};
    if(!Pages::TryGetPageRange(q.begin,q.bytes,r))return {};
    uint32_t cur=++epoch;
    if(cur==0){
      images.ForEach([](Id,const Image& im){im.query_epoch=0;});
      cur=++epoch;
    }
    Owners answer;
    Libs::Graphics::ForEachCanonicalOwnerPage<Skip>(
      pages,r,[&](size_t, const Owners& owners){
        ++emitted_pages;
        owners.ForEach([&](Id id){
          ++inspected_owners;
          auto* image=images.try_get(id);
          if(image==nullptr||image->query_epoch==cur)return;
          image->query_epoch=cur;
          if(Overlaps(*image,q))answer.push_back(id);
        });
      });
    return answer;
  }
  Owners Oracle(const Query& q) const {
    Pages::PageRange r{};
    if(!Pages::TryGetPageRange(q.begin,q.bytes,r))return {};
    Owners out;
    for(size_t page=r.first;page<r.last_exclusive;++page){
      for(Id id:active){
        const auto* im=images.try_get(id);
        Check(im!=nullptr,"oracle active");
        const size_t first=static_cast<size_t>(im->begin>>20);
        const size_t last=static_cast<size_t>(((im->begin+im->bytes-1)>>20)+1);
        if(page<first||page>=last||out.Contains(id))continue;
        if(Overlaps(*im,q))out.push_back(id);
      }
    }
    return out;
  }
};
bool Equal(const Owners& a,const Owners& b){
  if(a.size()!=b.size())return false;
  for(size_t i=0;i<a.size();++i)if(a[i]!=b[i])return false;
  return true;
}
uint64_t Hash(uint64_t h,const Owners& ids){
  for(const Id id:ids){
    h^=(uint64_t(id.generation)<<32)|id.index;
    h*=1099511628211ull;
  }
  h^=ids.size();
  return h*1099511628211ull;
}
void Preflight(const Workload& w) {
  Model m;
  for(const auto spec:w.specs)m.Register(spec);
  const auto check=[&](const Query& q){
    auto oracle=m.Oracle(q);
    auto legacy=m.QueryPage<false>(q);
    auto skip=m.QueryPage<true>(q);
    Check(Equal(legacy,oracle),"legacy matches independently ordered oracle");
    Check(Equal(skip,oracle),"bucket skip matches independently ordered oracle");
  };
  for(const auto& q:w.queries)check(q);
  const size_t buckets=m.pages.AllocatedBucketCount();
  m.Churn();
  for(const auto& q:w.queries)check(q);
  Check(m.pages.AllocatedBucketCount()==buckets,"unregister does not free buckets");
  Check(Equal(m.QueryPage<false>({0,0,false}),{}),"invalid query empty legacy");
  Check(Equal(m.QueryPage<true>({Limit-1,2,false}),{}),
        "invalid cross-address-limit query empty optimized");
  // Previously allocated-and-now-empty L1 bucket is semantically harmless;
  // the skip algorithm MUST continue to enumerate the bucket's empty pages.
  Model transient;
  const auto lone=transient.Register({Base,4096});
  const auto old_count=transient.pages.AllocatedBucketCount();
  transient.Unregister(lone);
  Check(transient.pages.AllocatedBucketCount()==old_count,"empty allocated bucket retained");
  Check(Equal(transient.QueryPage<false>({Base,8*GiB,false}),
              transient.QueryPage<true>({Base,8*GiB,false})),
        "retained allocated empty bucket equivalence");
}
struct Replay{
  uint64_t registration_ns=0,query_ns=0,churn_ns=0,post_query_ns=0,checksum=1469598103934665603ull;
  uint64_t new_requests=0;
  size_t buckets=0,owner_visits=0,emitted_pages=0;
};
template<bool Skip> Replay Run(const Workload& w){
  Model m;
  Replay out;
  const auto before=alloc_requests;
  const auto t=Clock::now();
  for(const auto spec:w.specs)m.Register(spec);
  out.registration_ns=Ns(t);
  const auto q0=Clock::now();
  for(const auto& q:w.queries)out.checksum=Hash(out.checksum,m.QueryPage<Skip>(q));
  out.query_ns=Ns(q0);
  const auto mutation=Clock::now();
  m.Churn();
  out.churn_ns=Ns(mutation);
  const auto q1=Clock::now();
  for(const auto& q:w.queries)out.checksum=Hash(out.checksum,m.QueryPage<Skip>(q));
  out.post_query_ns=Ns(q1);
  out.buckets=m.pages.AllocatedBucketCount();
  out.owner_visits=m.inspected_owners;
  out.emitted_pages=m.emitted_pages;
  out.new_requests=alloc_requests-before;
  return out;
}
} // namespace

// Only operator-new REQUESTS, not bytes, RSS, driver allocations or C++ live peak.
// No custom allocation behavior beyond delegating to malloc/free.
void* operator new(std::size_t n) {
  void* p=std::malloc(n?n:1);if(!p)throw std::bad_alloc{};
  CountNew();return p;
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}

int main(int argc,char** argv){
  if(argc!=4){std::fputs("usage: canonical_bucket_walk_bench scenario(0..3) mode(0|1) rep(0..2)\n",stderr);return 2;}
  const int scenario=std::atoi(argv[1]),mode=std::atoi(argv[2]),rep=std::atoi(argv[3]);
  if(scenario<0||scenario>3||mode<0||mode>1||rep<0||rep>2)return 2;
  static_assert(sizeof(Id)==8);
  const auto w=Make(scenario);
  Preflight(w);
  count_allocations=true;
  auto row=mode==0?Run<false>(w):Run<true>(w);
  count_allocations=false;
  const char* scenarios[]={"dense_overlap","sparse_multibucket","churn_buckets","boundary_44bit"};
  const char* modes[]={"canonical_page_find","canonical_l1_bucket_skip"};
  std::printf("%s,%s,%d,%zu,%zu,%llu,%llu,%llu,%llu,%llu,%zu,%zu,%zu\n",
       scenarios[scenario],modes[mode],rep,w.specs.size(),w.queries.size()*2,
       static_cast<unsigned long long>(row.registration_ns),
       static_cast<unsigned long long>(row.query_ns),
       static_cast<unsigned long long>(row.churn_ns),
       static_cast<unsigned long long>(row.post_query_ns),
       static_cast<unsigned long long>(row.checksum),
       row.buckets,row.owner_visits,row.new_requests);
  return 0;
}
