// KYTY-INDEX-001 G2-B0: independent CPU-only GC/lifetime model.
// Never linked to kyty_emulator. Candidate is deliberately more conservative
// than existing production dirty/invalidation semantics.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <vector>

namespace {
struct Id {
  uint32_t index = UINT32_MAX;
  uint32_t generation = 0;
  friend bool operator==(Id, Id) = default;
};
struct Item {
  uint32_t generation = 1;
  bool live = false, registered = false, queued = false;
  bool gpu_dirty = false, cpu_dirty = false, buffer_dirty = false;
  bool tiled = false, can_readback = true;
  Id parent{};
  uint64_t last_touch = 0, bytes = 4096;
  uint32_t views = 1;
};
struct Pending { Id id; uint64_t gpu_tick; };
struct Config {
  uint64_t trigger = 10 * 4096, pressure = 20 * 4096, critical = 40 * 4096;
  size_t object_trigger = 4, view_trigger = 8; // test values, NOT device limits
};
struct Result {
  std::vector<Id> roots;
  size_t scanned = 0;
  bool memory_triggered = false, object_triggered = false;
};
class World {
public:
  std::vector<Item> slots;
  std::deque<Pending> pending;
  uint64_t submitted = 0, completed = 0, gc_tick = 100;
  uint64_t used = 0; // synthetic registered guest bytes, NOT VkDeviceMemory
  Config cfg{};
  Item* Get(Id id) {
    return id.index < slots.size() && slots[id.index].live &&
           slots[id.index].generation == id.generation ? &slots[id.index] : nullptr;
  }
  const Item* Get(Id id) const {
    return id.index < slots.size() && slots[id.index].live &&
           slots[id.index].generation == id.generation ? &slots[id.index] : nullptr;
  }
  Id Add(uint64_t bytes = 4096, uint32_t views = 1, Id parent = {}) {
    if (parent.generation) {
      assert(Get(parent) && Get(parent)->registered && !Get(parent)->parent.generation);
    }
    uint32_t idx = 0;
    while (idx < slots.size() && slots[idx].live) ++idx;
    if (idx == slots.size()) slots.emplace_back();
    auto& s = slots[idx];
    const auto gen = s.generation;
    s = Item{};
    s.generation = gen;
    s.live = s.registered = true;
    s.bytes = bytes;
    s.views = views;
    s.parent = parent;
    s.last_touch = gc_tick;
    used += bytes;
    return {idx, gen};
  }
  size_t Live() const {
    return static_cast<size_t>(std::count_if(slots.begin(), slots.end(),
        [](const Item& s) { return s.live; }));
  }
  size_t Views() const {
    size_t result = 0;
    for (const auto& s : slots) if (s.live) result += s.views;
    return result;
  }
  size_t Registered() const {
    return static_cast<size_t>(std::count_if(slots.begin(), slots.end(),
        [](const Item& s) { return s.registered; }));
  }
  bool CleanClosure(Id id) const {
    const auto* root = Get(id);
    if (!root || !root->registered || root->parent.generation || root->gpu_dirty)
      return false;
    for (const auto& child : slots) {
      if (child.registered && child.parent == id && child.gpu_dirty)
        return false;
    }
    return true;
  }
  void RetireOne(Id id) {
    auto* s = Get(id);
    assert(s && s->registered && !s->queued && used >= s->bytes);
    used -= s->bytes;
    s->registered = false;
    s->queued = true;
    pending.push_back({id, submitted});
  }
  void RetireRoot(Id root) {
    assert(CleanClosure(root));
    // Associated stencil objects retire before their root.
    std::vector<Id> children;
    for (uint32_t i = 0; i < slots.size(); ++i) {
      if (slots[i].registered && slots[i].parent == root)
        children.push_back({i, slots[i].generation});
    }
    for (Id id : children) RetireOne(id);
    RetireOne(root);
  }
  void FinishGpu(uint64_t tick) {
    assert(completed <= tick && tick <= submitted);
    completed = tick;
    while (!pending.empty() && pending.front().gpu_tick <= completed) {
      const Id id = pending.front().id;
      auto* s = Get(id);
      assert(s && s->queued && !s->registered);
      assert(pending.front().gpu_tick <= completed);
      uint32_t gen = s->generation + 1;
      if (gen == 0) gen = 1;
      *s = Item{};
      s->generation = gen;
      pending.pop_front();
    }
  }
  void Verify() const {
    uint64_t accounted = 0;
    size_t queued = 0;
    for (const auto& s : slots) {
      if (!s.live) {
        assert(!s.registered && !s.queued);
        continue;
      }
      assert(s.generation != 0);
      if (s.registered) accounted += s.bytes;
      if (s.queued) { ++queued; assert(!s.registered); }
      else assert(s.registered);
      if (s.registered && s.parent.generation) {
        const auto* parent = Get(s.parent);
        assert(parent && parent->registered && !parent->parent.generation);
      }
    }
    assert(used == accounted && queued == pending.size());
    for (const auto& p : pending) {
      const auto* s = Get(p.id);
      assert(s && s->queued && !s->registered && p.gpu_tick <= submitted);
    }
  }
};

// Independent brute-force selector: take oldest eligible candidates,
// THEN filter protected/stencil entries. A skip still consumes a GC slot.
std::vector<Id> ReferenceEligible(const World& w, uint64_t tick,
                                  size_t budget, uint64_t age) {
  std::vector<Id> candidates;
  for (uint32_t i = 0; i < w.slots.size(); ++i) {
    const auto& s = w.slots[i];
    if (s.registered && s.last_touch <= tick && tick - s.last_touch >= age)
      candidates.push_back({i, s.generation});
  }
  std::sort(candidates.begin(), candidates.end(), [&](Id a, Id b) {
    const auto& l = w.slots[a.index];
    const auto& r = w.slots[b.index];
    if (l.last_touch != r.last_touch) return l.last_touch < r.last_touch;
    return a.index < b.index;
  });
  if (candidates.size() > budget) candidates.resize(budget);
  candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](Id id) {
    return w.slots[id.index].parent.generation || !w.CleanClosure(id);
  }), candidates.end());
  return candidates;
}

// Test-only selector: keep production candidate ages/quotas and the conditional
// second critical pass, but NEVER schedule an unproven GPU writeback.
Result Collect(World& w, bool dual) {
  const uint64_t tick = w.gc_tick++;
  Result result;
  result.memory_triggered = w.used >= w.cfg.trigger;
  result.object_triggered = dual &&
      (w.Live() >= w.cfg.object_trigger || w.Views() >= w.cfg.view_trigger);
  if (!result.memory_triggered && !result.object_triggered) return result;
  const auto pass = [&](bool allow_aggressive) {
    bool pressured = w.used >= w.cfg.pressure;
    bool aggressive = allow_aggressive && w.used >= w.cfg.critical;
    const uint64_t age = std::min<uint64_t>(
        aggressive ? 160 : pressured ? 80 : 16, tick);
    size_t remaining = aggressive ? 40u : pressured ? 20u : 10u;
    std::vector<Id> candidates;
    for (uint32_t i = 0; i < w.slots.size(); ++i) {
      const auto& s = w.slots[i];
      if (s.registered && s.last_touch <= tick - age)
        candidates.push_back({i, s.generation});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [&](Id a, Id b) {
      if (w.slots[a.index].last_touch != w.slots[b.index].last_touch)
        return w.slots[a.index].last_touch < w.slots[b.index].last_touch;
      return a.index < b.index;
    });
    if (candidates.size() > remaining) candidates.resize(remaining);
    for (Id id : candidates) {
      if (!remaining) break;
      --remaining; // skipped candidates also consume budget
      const auto* s = w.Get(id);
      if (!s || !s->registered) continue;
      ++result.scanned;
      if (s->parent.generation || !w.CleanClosure(id)) continue;
      result.roots.push_back(id);
      w.RetireRoot(id);
      if (aggressive && w.used < w.cfg.critical) {
        remaining >>= 2;
        aggressive = false;
      }
      if (pressured && w.used < w.cfg.pressure) {
        remaining >>= 1;
        pressured = false;
      }
    }
  };
  pass(false);
  if (w.used >= w.cfg.critical) pass(true);
  w.Verify();
  return result;
}

// Independent transition checker. Detect dirty loss, orphaned stencil, a
// premature slot erase/reuse and a deletion without the deferred queue.
bool SafeTransition(const World& before, const World& after) {
  if (before.slots.size() != after.slots.size()) return false;
  for (uint32_t i = 0; i < before.slots.size(); ++i) {
    const auto& old = before.slots[i];
    const auto& cur = after.slots[i];
    if (!old.live) continue;
    if (!cur.live || old.generation != cur.generation) return false;
    if (old.registered && !cur.registered) {
      if (!cur.queued || old.gpu_dirty) return false;
      if (old.parent.generation) {
        const auto* parent = after.Get(old.parent);
        if (!parent || parent->registered) return false;
      }
    }
    if (old.registered && cur.registered && old.parent.generation) {
      const auto* parent = after.Get(old.parent);
      if (!parent || !parent->registered) return false;
    }
  }
  return true;
}

uint64_t checks = 0;
void Check(bool ok) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "G2-B0 failed at check %llu\n",
                 static_cast<unsigned long long>(checks));
    std::abort();
  }
}
void BaselineParity() {
  for (int mask = 0; mask < 512; ++mask) {
    World a;
    a.cfg.trigger = 4 * 4096;
    a.cfg.object_trigger = a.cfg.view_trigger = 100;
    for (int i = 0; i < 4; ++i) {
      const Id id = a.Add();
      a.Get(id)->last_touch = (mask & (1 << i)) ? 99u : static_cast<uint64_t>(i);
      a.Get(id)->gpu_dirty = (mask & (1 << (i + 4))) != 0;
    }
    World b = a;
    const auto x = Collect(a, false);
    const auto y = Collect(b, true);
    Check(x.roots == y.roots && a.Registered() == b.Registered());
    Check(SafeTransition(b, a) && SafeTransition(b, b));
  }
}
void IndependentSelector() {
  for (int mask = 0; mask < 1024; ++mask) {
    World w;
    w.cfg.trigger = 1000000;
    w.cfg.object_trigger = 3;
    w.cfg.view_trigger = 100;
    for (int i = 0; i < 5; ++i) {
      const Id id = w.Add();
      w.Get(id)->last_touch = (mask & (1 << i)) ? 96u : static_cast<uint64_t>(i);
      w.Get(id)->gpu_dirty = (mask & (1 << (i + 5))) != 0;
    }
    const auto expected = ReferenceEligible(w, 100, 10, 16);
    const World before = w;
    const auto r = Collect(w, true);
    Check(r.object_triggered && !r.memory_triggered);
    Check(r.roots == expected);
    Check(SafeTransition(before, w));
    Check(w.pending.size() == expected.size());
  }
}
void FencesAndGeneration() {
  World w;
  w.cfg.trigger = 1ull << 30;
  w.cfg.object_trigger = 1;
  w.submitted = 42;
  const Id root = w.Add(4096, 3);
  const Id child = w.Add(4096, 2, root);
  w.Get(root)->last_touch = w.Get(child)->last_touch = 1;
  const auto r = Collect(w, true);
  Check(r.roots.size() == 1 && r.roots.front() == root);
  Check(w.pending.size() == 2 && w.Live() == 2 && w.Registered() == 0);
  const Id fresh = w.Add();
  Check(fresh.index != root.index && fresh.index != child.index);
  w.FinishGpu(41);
  Check(w.Live() == 3 && w.pending.size() == 2);
  w.FinishGpu(42);
  Check(w.pending.empty() && w.Live() == 1 && !w.Get(root) && !w.Get(child));
  const Id reused = w.Add();
  Check((reused.index == root.index || reused.index == child.index) &&
        reused.generation != (reused.index == root.index
            ? root.generation : child.generation));
  w.Verify();
}
void DirtyClosureMutants() {
  for (uint32_t bits = 0; bits < 256; ++bits) {
    World w;
    w.cfg.trigger = 1ull << 30;
    w.cfg.object_trigger = 1;
    const Id root = w.Add();
    const Id child = w.Add(4096, 2, root);
    w.Get(root)->last_touch = w.Get(child)->last_touch = 0;
    w.Get(root)->gpu_dirty = (bits & 1u) != 0;
    w.Get(child)->gpu_dirty = (bits & 2u) != 0;
    w.Get(root)->cpu_dirty = (bits & 4u) != 0;
    w.Get(root)->buffer_dirty = (bits & 8u) != 0;
    w.Get(root)->tiled = (bits & 16u) != 0;
    w.Get(child)->tiled = (bits & 32u) != 0;
    w.Get(root)->can_readback = (bits & 64u) != 0;
    w.Get(child)->can_readback = (bits & 128u) != 0;
    const World before = w;
    const auto r = Collect(w, true);
    Check(r.roots.empty() == ((bits & 3u) != 0));
    Check(SafeTransition(before, w));
    w.Verify();
    if (bits & 3u) {
      World bad = before;
      bad.RetireOne(child);
      bad.RetireOne(root); // mutant ignores dirty flag
      Check(!SafeTransition(before, bad));
    }
  }
}
void BudgetAndCritical() {
  {
    World w;
    w.cfg.trigger = 1ull << 30;
    w.cfg.object_trigger = 1;
    for (int i = 0; i < 11; ++i) {
      const Id id = w.Add();
      w.Get(id)->last_touch = 0;
      w.Get(id)->gpu_dirty = i < 10;
    }
    const auto r = Collect(w, true);
    Check(r.scanned == 10 && r.roots.empty() && w.Registered() == 11);
  }
  {
    World w;
    w.gc_tick = 200;
    w.cfg.trigger = 10 * 4096;
    for (int i = 0; i < 45; ++i) {
      const Id id = w.Add();
      w.Get(id)->last_touch = 0;
      w.Get(id)->gpu_dirty = i < 20;
    }
    const World before = w;
    const auto r = Collect(w, true);
    Check(r.memory_triggered && r.scanned > 20 && !r.roots.empty());
    Check(SafeTransition(before, w));
  }
}
void NegativeControls() {
  World w;
  w.submitted = 7;
  w.cfg.object_trigger = 1;
  w.cfg.trigger = 1ull << 30;
  const Id root = w.Add();
  const Id child = w.Add(4096, 1, root);
  w.Get(root)->last_touch = w.Get(child)->last_touch = 0;
  const World before = w;
  World orphan = w;
  orphan.RetireOne(root); // mutant: leaves registered stencil orphan
  Check(!SafeTransition(before, orphan));
  Collect(w, true);
  World premature = w;
  premature.slots[root.index] = Item{}; // mutant: physical free before fence
  Check(!SafeTransition(w, premature));
}
void PressureAndRecency() {
  World w;
  w.cfg.trigger = 1ull << 30;
  w.cfg.object_trigger = 4;
  w.cfg.view_trigger = 8;
  const Id old = w.Add(4096, 1);
  w.Get(old)->last_touch = 0;
  Check(Collect(w, true).roots.empty());
  const Id recent = w.Add(4096, 9);
  w.Get(recent)->last_touch = w.gc_tick;
  const auto r = Collect(w, true);
  Check(r.object_triggered && r.roots.size() == 1 && r.roots.front() == old);
  Check(w.Get(recent) && w.Get(recent)->registered);
}
void DeterministicReplay() {
  uint32_t state = 0xA11CE202u;
  auto draw = [&] { state ^= state << 13; state ^= state >> 17;
                    state ^= state << 5; return state; };
  for (int replay = 0; replay < 64; ++replay) {
    World w;
    w.cfg.trigger = 7 * 4096;
    w.cfg.object_trigger = 4;
    w.cfg.view_trigger = 8;
    for (int step = 0; step < 180; ++step) {
      switch (draw() % 5) {
        case 0: case 1: if (w.Live() < 50) {
          const Id id = w.Add(4096, 1 + draw() % 4);
          auto* s = w.Get(id);
          s->last_touch = (draw() % 2) ? 0 : w.gc_tick;
          s->gpu_dirty = (draw() % 4) == 0;
        } break;
        case 2: {
          const World before = w;
          const auto r = Collect(w, true);
          Check(SafeTransition(before, w));
          if (!r.memory_triggered && !r.object_triggered) Check(r.roots.empty());
          break;
        }
        case 3: {
          ++w.submitted;
          const uint64_t up_to = w.submitted - (draw() % 2);
          if (up_to >= w.completed) w.FinishGpu(up_to);
          break;
        }
        case 4:
          for (auto& s : w.slots)
            if (s.registered && (draw() & 7u) == 0) s.last_touch = w.gc_tick;
          break;
      }
      w.Verify();
    }
    w.FinishGpu(w.submitted);
    w.Verify();
  }
}
} // namespace
int main() {
  BaselineParity();
  IndependentSelector();
  FencesAndGeneration();
  DirtyClosureMutants();
  BudgetAndCritical();
  NegativeControls();
  PressureAndRecency();
  DeterministicReplay();
  std::printf("TextureGcPolicyOracleTests: PASS checks=%llu\n",
              static_cast<unsigned long long>(checks));
  std::puts("decision=OFFLINE_ONLY;production_gc=NO_GO;gpu_timing=NOT_MEASURED");
}
