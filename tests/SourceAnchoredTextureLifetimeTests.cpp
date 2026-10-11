// KYTY-INDEX-001 G2-B2: source-anchored CPU-only regression for the REAL
// production SlotVector and LeastRecentlyUsedCache implementations.
// Fake deferred callbacks model scheduler completion; they do NOT construct
// CommandScheduler or claim to test real GPU synchronization.
#include "common/slotVector.h"
#include "common/lruCache.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <unordered_set>
#include <vector>

namespace {
using Id = Common::SlotId;

uint64_t checks = 0, queries = 0, operations = 0;
void Check(bool ok, const char* label) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "G2-B2 failure: %s operation=%llu\n",
      label, static_cast<unsigned long long>(operations));
    std::abort();
  }
}
struct Payload {
  int* destroyed = nullptr;
  uint32_t label = 0;
  bool registered = true;
  size_t lru_id = 0;
  uint64_t last_touch = 0;

  explicit Payload(int* count, uint32_t n) : destroyed(count), label(n) {}
  Payload(const Payload&) = delete;
  Payload& operator=(const Payload&) = delete;
  ~Payload() { if (destroyed) ++*destroyed; }
};

// Test A: direct use of production SlotVector: stable pointer, deferred logical
// state, count/capacity, destructor timing, hash and stale-generation rejection.
void TestRealSlots() {
  int destroyed = 0;
  Common::SlotVector<Payload> slots;
  const Id a = slots.insert(&destroyed, 10);
  const Id b = slots.insert(&destroyed, 20);
  Check(slots.size() == 2 && slots.capacity() == 2, "real slot occupancy");
  const Payload* stable = slots.try_get(b);
  Check(stable != nullptr && stable->label == 20, "original slot");
  Check(slots.try_get(Id {a.index, a.generation + 1}) == nullptr,
        "forged future generation rejected");
  slots[a].registered = false; // exact logical state is distinct from occupancy
  Check(slots.try_get(a) && !slots.try_get(a)->registered &&
        slots.size() == 2 && destroyed == 0, "unregistered slot retained");
  const Id c = slots.insert(&destroyed, 30);
  Check(c.index != a.index && c.index != b.index, "pending slot not reused");
  Check(slots.try_get(b) == stable, "deque-stable object identity");
  uint32_t visits = 0;
  slots.ForEach([&](Id id, const Payload& item) {
    Check(slots.try_get(id) == &item, "ForEach ID exact generation");
    ++visits;
  });
  Check(visits == 3, "ForEach counts logically dead but physically allocated");
  slots.erase(a);
  Check(destroyed == 1 && !slots.try_get(a), "physical erase invokes destructor");
  const Id recycled = slots.insert(&destroyed, 40);
  Check(recycled.index == a.index && recycled.generation == a.generation + 1,
        "actual generation increment and LIFO recycle");
  Check(!slots.try_get(a) && slots.try_get(recycled) &&
        slots.try_get(recycled)->label == 40, "ABA-resilient retrieval");
  std::unordered_set<Id> identities{a, b, c, recycled};
  Check(identities.size() == 4, "real SlotId equality and hashing");
  const auto& readonly = slots;
  visits = 0;
  readonly.ForEach([&](Id id, const Payload&) {
    Check(readonly.try_get(id) != nullptr, "const enumeration");
    ++visits;
  });
  Check(visits == 3 && slots.capacity() == 3, "actual capacity is slot count");
  slots.erase(b);
  slots.erase(c);
  slots.erase(recycled);
  Check(slots.size() == 0 && destroyed == 4, "every physical erase exactly once");
}

// Test B: store the *real* SlotVector IDs inside the *real* LRU, then compare
// its aged-first traversal with a standalone vector reference. Unique monotone
// touch ticks remove ambiguous equal-tick tie ordering.
void TestRealLruDifferential() {
  uint32_t random = 0x62B2A11Cu;
  auto draw = [&] {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  for (unsigned run = 0; run < 64; ++run) {
    int destroyed = 0;
    Common::SlotVector<Payload> slots;
    Common::LeastRecentlyUsedCache<Id, uint64_t> lru;
    struct Expected { Id id; uint64_t tick; bool active; };
    std::vector<Expected> oracle;
    uint64_t tick = 1;
    for (unsigned step = 0; step < 160; ++step) {
      ++operations;
      std::vector<size_t> live;
      for (size_t i = 0; i < oracle.size(); ++i)
        if (oracle[i].active) live.push_back(i);
      const uint32_t choice = draw() % 7;
      if ((choice < 3 && live.size() < 20) || live.empty()) {
        Id id = slots.insert(&destroyed, static_cast<uint32_t>(step + 1));
        Payload& item = slots[id];
        item.last_touch = tick;
        item.lru_id = lru.Insert(id, tick++);
        oracle.push_back({id, item.last_touch, true});
      } else if (choice < 5) {
        const size_t index = live[draw() % live.size()];
        auto& ref = oracle[index];
        auto* item = slots.try_get(ref.id);
        Check(item && item->registered, "LRU touch allocated");
        lru.Touch(item->lru_id, tick);
        item->last_touch = tick;
        ref.tick = tick++;
      } else {
        const size_t index = live[draw() % live.size()];
        auto& ref = oracle[index];
        auto* item = slots.try_get(ref.id);
        Check(item != nullptr, "LRU release allocated");
        lru.Free(item->lru_id);
        slots.erase(ref.id);
        ref.active = false;
      }
      const uint64_t cutoff = tick > 16 ? tick - 16 : 0;
      const size_t limit = 1 + draw() % 10;
      std::vector<Id> observed, expected;
      lru.ForEachItemBelow(cutoff, [&](Id id) {
        observed.push_back(id);
        return observed.size() == limit;
      });
      std::vector<Expected> old;
      for (const auto& e : oracle)
        if (e.active && e.tick <= cutoff) old.push_back(e);
      std::sort(old.begin(), old.end(), [](const auto& a, const auto& b) {
        return a.tick < b.tick;
      });
      for (size_t i = 0; i < old.size() && i < limit; ++i)
        expected.push_back(old[i].id);
      Check(observed == expected, "real LRU and independent tick oracle agree");
      ++queries;
      size_t physically_allocated = 0;
      slots.ForEach([&](Id id, const Payload&) {
        Check(slots.try_get(id) != nullptr, "real SlotVector traversal");
        ++physically_allocated;
      });
      size_t expected_allocated = 0;
      for (const auto& entry : oracle)
        if (entry.active) ++expected_allocated;
      Check(physically_allocated == expected_allocated &&
            physically_allocated == slots.size(), "slot/reference count invariant");
    }
    for (auto& e : oracle) if (e.active) {
      const Payload* p = slots.try_get(e.id);
      Check(p != nullptr, "LRU cleanup target");
      lru.Free(p->lru_id);
      slots.erase(e.id);
      e.active = false;
    }
    Check(slots.size() == 0, "LRU run releases all real slots");
  }
}

// Test C: controlled deferred callback queue, but with actual production
// SlotVector::erase and actual LRU Free. No real scheduler or GPU created.
struct DeferredHarness {
  int destroyed = 0;
  Common::SlotVector<Payload> slots;
  Common::LeastRecentlyUsedCache<Id, uint64_t> lru;
  struct Pending { Id id; uint64_t tick; };
  std::deque<Pending> queue;
  uint64_t submitted = 0, completed = 0;
  uint64_t callback_completions = 0, queued = 0;
  Id Create(uint32_t label) {
    Id id = slots.insert(&destroyed, label);
    slots[id].lru_id = lru.Insert(id, submitted);
    return id;
  }
  bool Retire(Id id) {
    auto* p = slots.try_get(id);
    if (!p || !p->registered) return false;
    p->registered = false;
    lru.Free(p->lru_id);
    queue.push_back({id, submitted});
    ++queued;
    return true;
  }
  void Advance(uint64_t t) {
    Check(t >= completed && t <= submitted, "valid synthetic GPU tick");
    completed = t;
  }
  void Poll() {
    while (!queue.empty() && queue.front().tick <= completed) {
      const auto id = queue.front().id;
      Check(slots.try_get(id) && !slots.try_get(id)->registered,
            "callback owns still-allocated slot");
      slots.erase(id);
      queue.pop_front();
      ++callback_completions;
    }
  }
};
void TestDeferredLifecycleOnActualSlots() {
  DeferredHarness h;
  const Id root = h.Create(1), stencil = h.Create(2);
  h.submitted = 5;
  Check(h.Retire(stencil) && h.Retire(root), "logical stencil before root");
  Check(!h.Retire(root) && !h.Retire(stencil), "double logical retire forbidden");
  Check(h.queue.size() == 2 && h.slots.size() == 2 && h.destroyed == 0,
        "retirement does not physically delete");
  const Id new_id = h.Create(3);
  Check(new_id.index != root.index && new_id.index != stencil.index,
        "deferred IDs cannot be recycled");
  h.Advance(4);
  h.Poll();
  Check(h.slots.size() == 3 && h.destroyed == 0 && h.queue.size() == 2,
        "no callback before captured tick");
  h.Advance(5);
  h.Poll();
  Check(h.destroyed == 2 && h.queue.empty() && h.callback_completions == 2,
        "callbacks invoke real SlotVector erase after fence");
  const Id recycled = h.Create(4);
  Check(recycled.index == root.index && recycled.generation != root.generation,
        "LIFO reuse after completed callback");
  Check(!h.slots.try_get(root) && !h.slots.try_get(stencil),
        "both deferred stale generations invalid");
  // Scheduler inactive: actual SlotVector erase occurs synchronously.
  h.lru.Free(h.slots[recycled].lru_id);
  h.lru.Free(h.slots[new_id].lru_id);
  h.slots.erase(recycled);
  h.slots.erase(new_id);
  Check(h.slots.size() == 0 && h.destroyed == 4, "sync destruction counted");
  Check(h.queued == h.callback_completions, "normal shutdown has no backlog");
}

} // namespace
int main() {
  TestRealSlots();
  TestRealLruDifferential();
  TestDeferredLifecycleOnActualSlots();
  std::printf("SourceAnchoredTextureLifetimeTests: PASS checks=%llu lru_queries=%llu operations=%llu\n",
    static_cast<unsigned long long>(checks),
    static_cast<unsigned long long>(queries),
    static_cast<unsigned long long>(operations));
  std::puts("source=production_slot_vector_and_lru;real_gpu_scheduler=NOT_EXECUTED;production_gc=NO_GO");
}
