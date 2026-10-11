// G3-D0 source-anchored synthetic descriptor heap model (NO Vulkan calls).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
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
struct Batch{unsigned amount;std::vector<Token> free;explicit Batch(unsigned n):amount(n){}};
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
};
class Heap{
  unsigned target,active=0;
  unsigned long long submitted=0,completed=0;
  std::vector<Pool> pools{Pool{}};
  std::deque<Pending> pending;
  std::map<unsigned,Batch> batches;
  std::set<std::tuple<unsigned,unsigned,unsigned>> seen;
  Stats s;
  bool Allocate(unsigned layout,Batch& b){
    for(;;){
      ++s.attempts;
      Pool& p=pools[active];
      if(Fits(p,layout,b.amount)){
        for(unsigned i=0;i<b.amount;++i)b.free.push_back({active,p.generation,p.issued++,layout});
        p.sets+=b.amount;
        for(unsigned i=0;i<4;++i)p.used[i]+=b.amount*Costs[layout][i];
        p.ledger.push_back({layout,b.amount});
        s.allocated+=b.amount;
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
    if(pending.front().tick<=completed){
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
    for(const auto& [layout,b]:batches)
      for(const auto& t:b.free)
        Check(layout==t.layout&&t.pool==active&&t.generation==pools[active].generation,
              "cached set cannot outlive owning pool generation");
  }
public:
  explicit Heap(unsigned batch):target(batch){Check(batch==1||batch==8||batch==16||batch==32,"policy");}
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
  return rnd%10<7?0:1+rnd%3;
}
}
int main(){
  Pool p;
  Check(!Fits(p,0,1025),"reject excessive set capacity");
  Check(!Fits(p,3,129),"reject storage image overflow");
  Check(Fits(p,3,128),"accept exact storage image budget");
  Check(!(9>=10),"reject premature GPU tick reset");
  Check(NativeBatch==32&&MaxSets==1024,"frozen production limits");
  Heap smoke(32);
  for(unsigned i=0;i<256;++i){smoke.Tick(i+1,0);smoke.Commit(3);}
  Check(smoke.Result().rotations>0,"pool rollover actually covered");
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
