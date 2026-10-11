// G3-D0/D1 source-anchored synthetic descriptor heap model (NO Vulkan calls).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>
namespace {
constexpr unsigned MaxSets=1024, NativeBatch=32;
constexpr std::array<unsigned,4> Caps{8192,8192,1024,1024};
constexpr std::array<std::array<unsigned,4>,4> Costs{{
  {{1,4,0,2}},{{32,0,1,0}},{{0,64,1,4}},{{4,1,8,1}}
}};
unsigned long long checks=0;
void Check(bool ok,const char* msg){
  ++checks;
  if(!ok){std::fprintf(stderr,"G3-D0 FAIL: %s (%llu)\n",msg,checks);std::abort();}
}
struct Allocation{unsigned layout,count;};
struct Pool{
  unsigned generation=1,issued=0,sets=0;
  std::array<unsigned,4> used{};
  std::vector<Allocation> ledger;
};
struct Token{unsigned pool,generation,serial,layout;};
// First-use-slim candidate warms only after one successful allocation.
struct Batch{unsigned amount;bool warmed=false;std::vector<Token> free;explicit Batch(unsigned n):amount(n){}};
struct Pending{unsigned pool,generation;unsigned long long tick;};
bool Fits(const Pool& p,unsigned layout,unsigned count){
  if(layout>=Costs.size()||!count||count>MaxSets-p.sets)return false;
  for(unsigned i=0;i<4;++i)
    if(Costs[layout][i]&&count>(Caps[i]-p.used[i])/Costs[layout][i])return false;
  return true;
}
void VerifyPool(const Pool& p){
  unsigned long long total=0;
  std::array<unsigned long long,4> usage{};
  for(auto a:p.ledger){
    Check(a.layout<Costs.size()&&a.count>0,"valid allocation ledger");
    total+=a.count;
    for(unsigned i=0;i<4;++i)usage[i]+=uint64_t(a.count)*Costs[a.layout][i];
  }
  Check(total==p.sets&&total<=MaxSets&&p.issued==total,"set capacity ledger");
  for(unsigned i=0;i<4;++i)Check(usage[i]==p.used[i]&&usage[i]<=Caps[i],"descriptor type ledger");
}
struct Stats{
  unsigned long long commits=0,allocated=0,attempts=0,retries=0;
  unsigned long long rotations=0,created=1,reused=0,stranded=0,peak_pending=0;
  unsigned long long discarded=0; // irrevocably stranded at previous pool rollovers
};
// Every allocated descriptor set is either issued, still free in the current
// pool's batch, or discarded when that pool was retired (m_sets.clear()).
bool Conserved(uint64_t allocated,uint64_t issued,uint64_t discarded,uint64_t current){
  if(issued>allocated||discarded>allocated-issued)return false;
  return current==allocated-issued-discarded;
}
bool CanReuse(uint64_t completed,uint64_t retirement_tick){
  return completed>=retirement_tick;
}
class Heap{
  unsigned target,active=0;
  bool adaptive=false;
  unsigned long long submitted=0,completed=0;
  std::vector<Pool> pools{Pool{}};
  std::deque<Pending> pending;
  std::map<unsigned,Batch> batches;
  std::set<std::tuple<unsigned,unsigned,unsigned>> seen;
  Stats s;
  bool Allocate(unsigned layout,Batch& b){
    // Counterfactual cold-8/warm-32: new layouts receive one small batch;
    // only a layout that exhausted its initial batch attempts the 32 batch.
    if(adaptive&&b.warmed)b.amount=NativeBatch;
    for(;;){
      ++s.attempts;
      Pool& p=pools[active];
      if(Fits(p,layout,b.amount)){
        for(unsigned i=0;i<b.amount;++i)b.free.push_back({active,p.generation,p.issued++,layout});
        p.sets+=b.amount;
        for(unsigned i=0;i<4;++i)p.used[i]+=b.amount*Costs[layout][i];
        p.ledger.push_back({layout,b.amount});
        s.allocated+=b.amount;
        b.warmed=true;
        return true;
      }
      ++s.retries;
      if(b.amount==1)return false;
      b.amount/=2; // production retries after out-of-pool or fragmentation
    }
  }
  void Rotate(){
    ++s.rotations;
    pending.push_back({active,pools[active].generation,submitted});
    if(CanReuse(completed,pending.front().tick)){
      const Pending p=pending.front();
      pending.pop_front();
      Check(p.tick<=completed&&pools[p.pool].generation==p.generation,"retirement fence");
      active=p.pool;
      pools[active]=Pool{};
      pools[active].generation=p.generation+1;
      ++s.reused;
    }else{
      active=static_cast<unsigned>(pools.size());
      pools.emplace_back();
      ++s.created;
    }
    // The old batches contain real already-allocated descriptor sets. Unlike
    // the old end-of-trial stranded metric, their loss must be cumulative.
    for(const auto& [layout,b]:batches){
      Check(layout<Costs.size(),"retired layout type exists");
      s.discarded+=b.free.size();
    }
    batches.clear(); // production discards all cached batches on rollover
    s.peak_pending=std::max(s.peak_pending,static_cast<unsigned long long>(pending.size()));
  }
  void Verify(){
    Check(active<pools.size(),"active pool valid");
    for(const auto& p:pools)VerifyPool(p);
    for(const auto& p:pending){
      Check(p.pool!=active&&p.pool<pools.size(),"pending pool distinct");
      Check(p.generation==pools[p.pool].generation,"pending generation preserved");
    }
    uint64_t free_count=0;
    for(const auto& [layout,b]:batches)
      for(const auto& t:b.free){
        Check(layout==t.layout&&t.pool==active&&t.generation==pools[active].generation,
              "cached set cannot outlive owning pool generation");
        ++free_count;
      }
    Check(Conserved(s.allocated,s.commits,s.discarded,free_count),
          "all allocated sets accounted: issued + discarded + current free");
  }
public:
  explicit Heap(unsigned batch,bool cold8warm32=false):target(batch),adaptive(cold8warm32){
    Check(batch==1||batch==8||batch==16||batch==32,"policy");
    Check(!adaptive||batch==8,"warm-32 candidate starts with cold batch=8");
  }
  void Tick(unsigned long long now,unsigned long long done){
    Check(now>=submitted&&done>=completed&&done<=now,"monotone GPU ticks");
    submitted=now;completed=done;
  }
  void Commit(unsigned layout){
    Check(layout<Costs.size(),"valid layout");
    auto& b=batches.try_emplace(layout,target).first->second;
    if(b.free.empty()&&!Allocate(layout,b)){
      Rotate();
      auto& fresh=batches.try_emplace(layout,target).first->second;
      Check(Allocate(layout,fresh),"new descriptor pool accepts set");
    }
    auto& current=batches.at(layout);
    Check(!current.free.empty(),"nonempty successful allocation");
    const Token t=current.free.back();current.free.pop_back();
    Check(t.pool==active&&t.generation==pools[active].generation&&t.layout==layout,
          "issued set belongs to current pool");
    Check(seen.insert({t.pool,t.generation,t.serial}).second,"duplicate descriptor set");
    ++s.commits;Verify();
  }
  Stats Result(){
    Verify();
    for(const auto& [unused,b]:batches)s.stranded+=b.free.size();
    return s;
  }
};
unsigned Choice(unsigned scenario,unsigned n,unsigned& rnd){
  rnd^=rnd<<13;rnd^=rnd>>17;rnd^=rnd<<5;
  if(scenario==0)return 0;
  if(scenario==1)return n%4;
  if(scenario==2)return (n/37)%4;
  if(scenario==4)return n%67==0 ? 1+(n/67)%3 : 3;
  return rnd%10<7?0:1+rnd%3;
}
}
int WasteReplay(){
  // Named cold/hot fixtures are SYNTHETIC, not a draw-call trace.
  const std::array<unsigned,5> batches{1,8,16,32,8};
  const std::array<const char*,5> labels{"fixed1","fixed8","fixed16",
                                          "fixed32","cold8_warm32"};
  std::puts("scenario,policy,initial_batch,adaptive,commits,allocated_sets,"
            "issued_sets,discarded_spares_on_rollover,current_cached_spares,"
            "allocation_attempts,half_retries,pool_rotations,created_pools,"
            "reused_pools,peak_pending_pools,decision");
  uint64_t aggregate_discarded=0, aggregate_issued=0;
  for(unsigned scenario=0;scenario<5;++scenario){
    for(size_t policy=0;policy<batches.size();++policy){
      const bool adaptive=policy==4;
      Heap h(batches[policy],adaptive);
      unsigned rng=0xD35C0011u;
      for(unsigned i=0;i<1536;++i){
        const uint64_t tick=i+1;
        h.Tick(tick,tick>75?tick-75:0);
        h.Commit(Choice(scenario,i,rng));
      }
      const auto s=h.Result();
      Check(Conserved(s.allocated,s.commits,s.discarded,s.stranded),
            "independent waste conservation at end of scenario");
      Check(s.allocated>=s.commits&&s.commits==1536,
            "synthetic workload exactly preserved between policies");
      std::printf("%u,%s,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,MODEL_ONLY\n",
          scenario,labels[policy],batches[policy],unsigned(adaptive),
          s.commits,s.allocated,s.commits,s.discarded,s.stranded,
          s.attempts,s.retries,s.rotations,s.created,s.reused,s.peak_pending);
      aggregate_discarded+=s.discarded;
      aggregate_issued+=s.commits;
    }
  }
  Check(aggregate_issued==5ull*5ull*1536ull,
        "identical commit count for all policy comparisons");
  std::printf("summary,checks,%llu,scenarios,5,policies,5,"
              "total_discarded,%llu,production_changes,0,"
              "decision,G3_D1_MODEL_ONLY,PASS\n",checks,
              static_cast<unsigned long long>(aggregate_discarded));
  return 0;
}
int main(int argc,char** argv){
  Pool p;
  Check(!Fits(p,0,1025),"reject excessive set capacity");
  Check(!Fits(p,3,129),"reject storage image overflow");
  Check(Fits(p,3,128),"accept exact storage image budget");
  Check(!CanReuse(9,10)&&CanReuse(10,10),
        "reject premature GPU tick reset");
  Check(NativeBatch==32&&MaxSets==1024,"frozen production limits");
  Heap smoke(32);
  for(unsigned i=0;i<256;++i){
    smoke.Tick(i+1,0);
    smoke.Commit(i==1?0:3); // a cold layout leaves spare sets at rollover
  }
  const auto smoke_stats=smoke.Result();
  Check(smoke_stats.rotations>0,"pool rollover actually covered");
  Check(smoke_stats.discarded>0,"adversarial cold-layout spare actually discarded");
  Check(Conserved(smoke_stats.allocated,smoke_stats.commits,
                  smoke_stats.discarded,smoke_stats.stranded),
        "smoke descriptor conservation");
  Check(!Conserved(smoke_stats.allocated,smoke_stats.commits,
                   smoke_stats.discarded-1,smoke_stats.stranded),
        "negative: lost retired-batch counter is detected");
  Check(!Conserved(smoke_stats.allocated,smoke_stats.commits,
                   smoke_stats.discarded,smoke_stats.stranded+1),
        "negative: double-accounted cached set rejected");
  if(argc==2&&std::strcmp(argv[1],"--waste")==0)return WasteReplay();
  if(argc!=1)return 2;
  std::puts("scenario,batch,commits,allocated,attempts,half_retries,rotations,created,reused,stranded,peak_pending,decision");
  for(unsigned scenario=0;scenario<4;++scenario){
    for(unsigned batch:{1u,8u,16u,32u}){
      Heap h(batch);
      unsigned rng=0xD35C0011u;
      for(unsigned i=0;i<1536;++i){
        const uint64_t tick=i+1;
        h.Tick(tick,tick>75?tick-75:0);
        h.Commit(Choice(scenario,i,rng));
      }
      const auto s=h.Result();
      std::printf("%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,MODEL_ONLY\n",
        scenario,batch,s.commits,s.allocated,s.attempts,s.retries,s.rotations,
        s.created,s.reused,s.stranded,s.peak_pending);
    }
  }
  std::printf("summary,checks,%llu,trials_per_scenario,1536,scenarios,4,"
              "policies,4,production_changes,0,decision,G3_MODEL_ONLY,PASS\n",checks);
  return 0;
}
