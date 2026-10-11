// G2-B1 CI-only finite-state adversarial lifetime/fault-injection oracle.
// Deliberately separate from kyty_emulator and from the G2-B0 selector.
// Synthetic GPU ticks and dirty bits are NOT production Vulkan writeback.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace {
constexpr uint32_t Invalid = UINT32_MAX;
struct Key {
  uint32_t index = Invalid;
  uint32_t generation = 0;
  friend bool operator==(Key, Key) = default;
};
enum class Phase : uint8_t { Free, Registered, Pending };
enum class Action : uint8_t { None, Create, Mark, Resolve, Use, Retire, Complete, Drain };
enum Hazard : uint8_t { Gpu = 1, Cpu = 2, Buffer = 4 };
struct Slot {
  Phase phase = Phase::Free;
  uint32_t generation = 1;
  Key parent{};
  uint8_t hazards = 0;
  uint64_t last_use = 0;
  uint32_t views = 1;
  uint64_t bytes = 4096;
};
struct Callback {
  Key id;
  uint64_t fence = 0;
  uint64_t sequence = 0;
};
struct State {
  std::vector<Slot> slots;
  std::deque<Callback> pending;
  uint64_t submitted = 0, completed = 0, next_sequence = 0;
  uint64_t queued = 0, finished = 0;
  uint64_t registered_bytes = 0, live_views = 0;

  Slot* Find(Key key) {
    if (!key.generation || key.index >= slots.size()) return nullptr;
    Slot& s = slots[key.index];
    return s.phase != Phase::Free && s.generation == key.generation ? &s : nullptr;
  }
  const Slot* Find(Key key) const {
    if (!key.generation || key.index >= slots.size()) return nullptr;
    const Slot& s = slots[key.index];
    return s.phase != Phase::Free && s.generation == key.generation ? &s : nullptr;
  }
  Key Create(Key parent = {}) {
    if (parent.generation) {
      const Slot* p = Find(parent);
      if (!p || p->phase != Phase::Registered || p->parent.generation) return {};
    }
    uint32_t index = 0;
    while (index < slots.size() && slots[index].phase != Phase::Free) ++index;
    if (index == slots.size()) {
      if (slots.size() >= 12) return {}; // bounded research automaton
      slots.emplace_back();
    }
    Slot& s = slots[index];
    const uint32_t generation = s.generation;
    s = Slot{};
    s.generation = generation;
    s.phase = Phase::Registered;
    s.parent = parent;
    registered_bytes += s.bytes;
    live_views += s.views;
    return {index, generation};
  }
  bool Mark(Key key, uint8_t hazards) {
    Slot* s = Find(key);
    if (!s || s->phase != Phase::Registered) return false;
    s->hazards |= hazards;
    return true;
  }
  bool Resolve(Key key) {
    Slot* s = Find(key);
    if (!s || s->phase != Phase::Registered) return false;
    // Explicitly assumed hazard resolution, not a simulated GPU download.
    s->hazards = 0;
    return true;
  }
  bool Use(Key key) {
    Slot* s = Find(key);
    if (!s || s->phase != Phase::Registered) return false;
    s->last_use = ++submitted;
    return true;
  }
  bool Retire(Key root) {
    const Slot* r = Find(root);
    if (!r || r->phase != Phase::Registered || r->parent.generation ||
        r->hazards != 0) return false;
    std::vector<Key> children;
    for (uint32_t i = 0; i < slots.size(); ++i) {
      const Slot& s = slots[i];
      if (s.phase == Phase::Registered && s.parent == root) {
        if (s.hazards) return false; // entire closure must be resolved
        children.push_back({i, s.generation});
      }
    }
    // Atomic logical closure, child-first physical callback queue.
    for (Key child : children) Enqueue(child);
    Enqueue(root);
    return true;
  }
  bool Complete(uint64_t tick) {
    if (tick < completed || tick > submitted) return false;
    completed = tick; // completion and callback execution are independent
    return true;
  }
  bool DrainOne() {
    if (pending.empty() || pending.front().fence > completed) return false;
    const Callback cb = pending.front();
    Slot* s = Find(cb.id);
    if (!s || s->phase != Phase::Pending || s->last_use > completed) return false;
    const uint32_t generation = s->generation == UINT32_MAX
        ? 1u : static_cast<uint32_t>(s->generation + 1);
    live_views -= s->views;
    *s = Slot{};
    s->generation = generation;
    pending.pop_front();
    ++finished;
    return true;
  }
  void ShutdownDrain() {
    (void)Complete(submitted);
    while (DrainOne()) {}
  }
private:
  void Enqueue(Key id) {
    Slot* s = Find(id);
    // Validated by Retire before mutation.
    s->phase = Phase::Pending;
    registered_bytes -= s->bytes;
    const uint64_t fence = submitted >= s->last_use ? submitted : s->last_use;
    pending.push_back({id, fence, ++next_sequence});
    ++queued;
  }
};

// INDEPENDENT verifier does not call any State helpers for queue, generation,
// byte counts, ancestry or retiring. It inspects full state and transition.
bool WellFormed(const State& s) {
  if (s.completed > s.submitted || s.finished > s.queued ||
      s.queued - s.finished != s.pending.size()) return false;
  uint64_t registered_bytes = 0, views = 0;
  std::vector<uint32_t> references(s.slots.size(), 0);
  uint64_t sequence = 0;
  for (const Callback& cb : s.pending) {
    if (!cb.id.generation || cb.id.index >= s.slots.size() ||
        cb.sequence <= sequence || cb.sequence > s.next_sequence ||
        cb.fence > s.submitted) return false;
    sequence = cb.sequence;
    const Slot& v = s.slots[cb.id.index];
    if (v.phase != Phase::Pending || v.generation != cb.id.generation ||
        cb.fence < v.last_use || ++references[cb.id.index] != 1)
      return false;
  }
  for (uint32_t i = 0; i < s.slots.size(); ++i) {
    const Slot& v = s.slots[i];
    if (!v.generation) return false;
    if (v.phase == Phase::Free) {
      if (references[i]) return false;
      continue;
    }
    if (v.last_use > s.submitted) return false;
    views += v.views;
    if (v.phase == Phase::Registered) {
      if (references[i]) return false;
      registered_bytes += v.bytes;
    } else if (v.phase == Phase::Pending) {
      if (references[i] != 1 || v.hazards) return false;
    } else return false;
    if (v.parent.generation) {
      if (v.parent.index >= s.slots.size() || v.parent.index == i) return false;
      const Slot& p = s.slots[v.parent.index];
      if (p.generation != v.parent.generation || p.parent.generation) return false;
      // Child registered => parent registered. Child pending => parent
      // pending (callbacks are enqueued child-first and drained FIFO).
      if (p.phase != v.phase) return false;
    }
  }
  return registered_bytes == s.registered_bytes && views == s.live_views;
}
uint32_t NextGeneration(uint32_t g) { return g == UINT32_MAX ? 1u : g + 1u; }

bool Transition(const State& before, const State& after, Action action) {
  if (!WellFormed(before) || !WellFormed(after) ||
      after.slots.size() < before.slots.size() ||
      after.completed < before.completed || after.submitted < before.submitted ||
      after.finished < before.finished || after.queued < before.queued)
    return false;
  if (after.slots.size() > before.slots.size() && action != Action::Create)
    return false;
  if (action != Action::Complete && after.completed != before.completed)
    return false;
  if (action != Action::Use && after.submitted != before.submitted)
    return false;
  if (action != Action::Retire && after.queued != before.queued)
    return false;
  if (action != Action::Drain && after.finished != before.finished)
    return false;
  for (size_t i = 0; i < after.slots.size(); ++i) {
    const Slot& n = after.slots[i];
    if (i == before.slots.size()) {
      if (action != Action::Create || n.phase != Phase::Registered ||
          n.generation != 1) return false;
      continue;
    }
    if (i >= before.slots.size()) return false; // at most one new slot/event
    const Slot& p = before.slots[i];
    if (p.phase == Phase::Free && n.phase == Phase::Registered) {
      if (action != Action::Create || p.generation != n.generation) return false;
    } else if (p.phase == Phase::Registered && n.phase == Phase::Pending) {
      if (action != Action::Retire || p.hazards || n.generation != p.generation ||
          n.parent != p.parent || n.last_use != p.last_use) return false;
    } else if (p.phase == Phase::Pending && n.phase == Phase::Free) {
      if (action != Action::Drain || n.generation != NextGeneration(p.generation))
        return false;
      bool matched = false;
      for (const Callback& cb : before.pending) {
        if (cb.id.index == i && cb.id.generation == p.generation &&
            cb.fence <= after.completed && cb.fence >= p.last_use) matched = true;
      }
      if (!matched) return false;
    } else if (p.phase == n.phase) {
      if (p.generation != n.generation || p.parent != n.parent ||
          p.bytes != n.bytes || p.views != n.views) return false;
      if (p.hazards != n.hazards &&
          action != Action::Mark && action != Action::Resolve) return false;
      if (p.last_use != n.last_use && action != Action::Use) return false;
    } else return false;
  }
  // Separate recorded callback ordering from the final slot shapes:
  // successful drain always removes the FIFO front, never a later entry.
  if (action == Action::Drain && after.finished > before.finished) {
    if (before.pending.empty() || after.pending.size() + 1 != before.pending.size())
      return false;
    for (size_t i = 0; i < after.pending.size(); ++i)
      if (!(after.pending[i].id == before.pending[i+1].id) ||
          after.pending[i].sequence != before.pending[i+1].sequence)
        return false;
  }
  return true;
}

uint64_t assertions = 0, steps = 0, traces = 0, rejected = 0;
void Require(bool yes, const char* reason) {
  ++assertions;
  if (!yes) { std::fprintf(stderr, "G2-B1 FAIL: %s step=%llu trace=%llu\n",
       reason, static_cast<unsigned long long>(steps),
       static_cast<unsigned long long>(traces)); std::exit(1); }
}
void Mutant(const char* name, bool rejection) {
  Require(rejection, name);
  ++rejected;
  std::printf("fault,%s,rejected\n", name);
}

void FaultInjection() {
  State clean;
  const Key root = clean.Create();
  const Key c1 = clean.Create(root);
  const Key c2 = clean.Create(root);
  Require(clean.Use(root) && clean.Use(c1) && clean.Use(c2), "setup GPU uses");
  Require(clean.Retire(root) && WellFormed(clean), "setup pending closure");
  State m = clean;
  m.pending.push_back(m.pending.front());
  m.pending.back().sequence = ++m.next_sequence;
  ++m.queued; // even with matching aggregate counters, duplicate identity fails
  Mutant("duplicate_callback", !WellFormed(m));
  m = clean;
  ++m.pending.front().id.generation;
  Mutant("stale_generation_callback", !WellFormed(m));
  m = clean;
  m.pending.front().fence = 0;
  Mutant("forged_pre_use_fence", !WellFormed(m));
  m = clean;
  std::swap(m.pending[0], m.pending[1]);
  Mutant("out_of_order_callback", !WellFormed(m));
  m = clean;
  m.pending.pop_front();
  ++m.finished; // aggregates balance; missing pending-identity still fails
  Mutant("missing_callback", !WellFormed(m));
  m = clean;
  m.slots[c1.index].hazards = Gpu;
  Mutant("gpu_dirty_pending", !WellFormed(m));
  m = clean;
  m.slots[c2.index].hazards = Buffer;
  Mutant("buffer_dirty_pending", !WellFormed(m));
  m = clean;
  m.slots[root.index].hazards = Cpu;
  Mutant("cpu_dirty_pending", !WellFormed(m));
  m = clean;
  m.slots[root.index].phase = Phase::Registered;
  Mutant("orphan_stencil", !WellFormed(m));
  m = clean;
  m.slots[root.index].phase = Phase::Free;
  Mutant("premature_free", !WellFormed(m));
  m = clean;
  ++m.registered_bytes;
  Mutant("registered_byte_corruption", !WellFormed(m));
  m = clean;
  ++m.live_views;
  Mutant("live_view_corruption", !WellFormed(m));
  m = clean;
  ++m.finished;
  Mutant("completion_without_callback", !WellFormed(m));
  m = clean;
  m.slots[c2.index].parent.generation++;
  Mutant("stale_parent_generation", !WellFormed(m));
  m = clean;
  m.slots[c2.index].last_use = m.submitted + 1;
  Mutant("use_after_submission", !WellFormed(m));
  // A syntactically valid after-state can still be reached through an
  // illegal dirty retirement; the independent TRANSITION checker catches it.
  State dirty;
  Key dr = dirty.Create();
  Key dc = dirty.Create(dr);
  Require(dirty.Mark(dc, Gpu), "setup dirty association");
  State forged = dirty;
  Require(forged.Resolve(dc) && forged.Retire(dr) && WellFormed(forged),
          "setup forged clean retirement");
  Mutant("laundered_dirty_retirement", !Transition(dirty, forged, Action::Retire));
  // Fabricate a *well-formed* free slot without completing the required
  // per-object last-use fence. State checker accepts shape; transition rejects.
  State early;
  Key er = early.Create();
  Require(early.Use(er) && early.Retire(er) && WellFormed(early),
          "setup early callback");
  State after = early;
  after.slots[er.index] = Slot{};
  after.slots[er.index].generation = er.generation + 1;
  after.pending.pop_front();
  after.finished++;
  after.live_views--;
  Require(WellFormed(after), "setup early callback shape");
  Mutant("callback_before_gpu_fence", !Transition(early, after, Action::Drain));
  // Completed callback is legal, but forgetting the generation increment
  // must be rejected as an invalid state transition (ABA hazard).
  Require(early.Complete(early.submitted), "completed GPU fence");
  State before = early;
  Require(early.DrainOne(), "completed callback");
  State wrong = early;
  wrong.slots[er.index].generation = er.generation;
  Require(WellFormed(wrong), "incorrect generation looks locally valid");
  Mutant("missing_generation_increment", !Transition(before, wrong, Action::Drain));
  // Deterministic forced wrap: actual SlotVector increments and skips zero.
  State wrap;
  const Key k0 = wrap.Create();
  wrap.slots[k0.index].generation = UINT32_MAX;
  const Key maxid{k0.index, UINT32_MAX};
  Require(wrap.Retire(maxid) && wrap.Complete(0) && wrap.DrainOne(), "wrap retirement");
  const Key reused = wrap.Create();
  Require(reused.index == maxid.index && reused.generation == 1 &&
          !wrap.Find(maxid) && WellFormed(wrap), "generation UINT32_MAX->1");
  // Double free/double retirement and use-after-retire are forbidden.
  State protected_state = clean;
  Require(!protected_state.Retire(root) && !protected_state.Use(c1) &&
          !protected_state.Mark(root, Gpu) && !protected_state.Resolve(root) &&
          WellFormed(protected_state), "no activity after logical retirement");
  Require(!clean.DrainOne(), "callback before completion blocked");
  Require(clean.Complete(clean.submitted), "completion granted");
  while (clean.DrainOne()) {}
  Require(clean.pending.empty() && WellFormed(clean) && clean.queued == clean.finished,
          "normal shutdown queue drained");
}

bool Step(State& w, Key root, Key child, unsigned a, Action& action) {
  switch (a) {
    case 0: action=Action::Use; return w.Use(root);
    case 1: action=Action::Use; return w.Use(child);
    case 2: action=Action::Mark; return w.Mark(root, Gpu);
    case 3: action=Action::Mark; return w.Mark(child, Buffer);
    case 4: action=Action::Resolve; return w.Resolve(root);
    case 5: action=Action::Resolve; return w.Resolve(child);
    case 6: action=Action::Retire; return w.Retire(root);
    case 7: action=Action::Complete; return w.Complete(w.submitted);
    case 8: action=Action::Drain; return w.DrainOne();
    default: action=Action::Create; return static_cast<bool>(w.Create().generation);
  }
}

void Exhaustive() {
  // 10^4 traces x 16 hazard masks; 4 events per trace.
  for (unsigned mask = 0; mask != 16; ++mask) {
    for (uint32_t script = 0; script != 10000; ++script) {
      State w;
      const Key root = w.Create();
      const Key child = w.Create(root);
      if (mask & 1) (void)w.Mark(root, Gpu);
      if (mask & 2) (void)w.Mark(root, Cpu);
      if (mask & 4) (void)w.Mark(child, Gpu);
      if (mask & 8) (void)w.Mark(child, Buffer);
      Require(WellFormed(w), "initial exhaustive state");
      uint32_t actions = script;
      for (unsigned depth = 0; depth < 4; ++depth) {
        const unsigned code = actions % 10;
        actions /= 10;
        State before = w;
        Action act = Action::None;
        const bool applied = Step(w, root, child, code, act);
        ++steps;
        Require(Transition(before, w, applied ? act : Action::None),
                "exhaustive transition");
      }
      w.ShutdownDrain();
      Require(WellFormed(w) && w.pending.empty() && w.queued == w.finished,
              "exhaustive quiescence");
      ++traces;
    }
  }
}
void Replay() {
  // Full CI only; deterministic seed, no host time, no graphics driver.
  uint32_t random = 0xB121D0F1u;
  auto draw = [&]() {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  for (unsigned lane = 0; lane < 48; ++lane) {
    State w;
    const Key root = w.Create();
    const Key child = w.Create(root);
    for (unsigned i=0; i<192; ++i) {
      const unsigned action_number = draw() % 10;
      Action act = Action::None;
      State before = w;
      const bool applied = Step(w,root,child,action_number,act);
      ++steps;
      Require(Transition(before,w,applied?act:Action::None),"seeded transition");
    }
    w.ShutdownDrain();
    Require(WellFormed(w) && w.pending.empty() && w.queued == w.finished,
            "seeded quiescence");
  }
}
} // namespace
int main() {
  std::puts("kind,name,value");
  FaultInjection();
  Exhaustive();
  Replay();
  Require(rejected == 18, "mutation detection coverage");
  std::printf("coverage,faults_rejected,%llu\n",
              static_cast<unsigned long long>(rejected));
  std::printf("coverage,exhaustive_traces,%llu\n",
              static_cast<unsigned long long>(traces));
  std::printf("coverage,transition_steps,%llu\n",
              static_cast<unsigned long long>(steps));
  std::printf("coverage,assertions,%llu\n",
              static_cast<unsigned long long>(assertions));
  std::puts("decision,production_gc,NO_GO");
  std::puts("summary,TextureGcLifecycleFaultTests_PASS,1");
}
