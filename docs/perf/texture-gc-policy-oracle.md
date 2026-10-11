# KYTY-INDEX-001 / G2-B0 — offline dual-pressure GC correctness oracle

Research log: [Mathlab #289](https://github.com/definitely-stable/Mathlab/issues/289).
Code branch: `MrFr3di/KytyPS5:perf/texture-range-index`. No merge or main/upstream modification.

## Research question and model

The real TextureCache GC is memory-triggered; G2-A/A1 telemetry can show slot/view and deferred-erase pressure, but no representative game demonstrates a bottleneck. G2-B0 models a hypothetical extra image/view-pressure trigger **offline** before any production change.

`tests/TextureGcPolicyOracleTests.cpp` is pure CPU C++20: generation-tagged slots, registered vs queued states, root/stencil associations, indexed views, synthetic memory accounting, LRU ticks, GPU submission ticks and deferred physical deletion. Synthetic **4-slot/8-view** triggers are fixtures, NOT GPU device limits or deployment defaults. The test is never linked into `kyty_emulator` and never invokes Vulkan.

The GC model mirrors the existing candidate age windows 16/80/160, budgets 10/20/40, an optional critical second pass, skip-consuming-budget rule and threshold-driven budget reductions. The synthetic memory input is registered-image guest bytes, not VMA/driver memory budget. An experimental candidate requires *all members of a root/stencil closure to be GPU-clean*. It does not simulate actual writeback, aliases, GPU invalidation, compression or true CPU/GPU coherency. This restriction is intentionally more conservative than the live `FreeImage` semantics; it does not demonstrate a production bug.

## Independent correctness predicates

- Under conditions without object/view pressure, memory-only baseline and candidate select identical roots within the conservative model (512 deterministic masks).
- An independently written reference selector enumerates and sorts all eligible registered LRU objects, consumes the candidate quota before filtering stencil/dirty objects, and compares selected clean roots against the candidate (1024 configurations).
- Boolean combinations cover GPU/CPU/buffer dirty, tiled and possible readback flags for root and stencil members; the object-pressure lane refuses every GPU-modified closure. No writeback is assumed.
- A root retirement logically retires registered depth/stencil associates, and the independent transition auditor rejects orphaned stencil objects.
- Physically deleting slots is forbidden before the modeled GPU completion tick; pending slots are not reusable, and `SlotId` generations change on reuse. Stale IDs must not resolve.
- The candidate budget is exhausted by protected objects just as by accepted candidates; the critical-memory second pass is covered.
- Deliberately incorrect mutants (GPU-dirty discard, orphaned stencil, premature destruction) MUST be rejected; seeded replay `0xA11CE202` exercises create, GC, touch and completion sequences deterministically.

This oracle and candidate share a modeled state but use independent selection/audit code. Passing is evidence for **finite-model properties**, not a mathematical proof of full Vulkan semantics, a production GC correctness certificate, or a speedup. The memory-only baseline in this test has the *same conservative GPU-clean restriction*, rather than production's additional ownership/dirty logic.

## Windows CI and acceptance

```powershell
clang-cl /nologo /std:c++20 /O2 /EHsc tests/TextureGcPolicyOracleTests.cpp /Fe:texture_gc_policy_oracle.exe
.\texture_gc_policy_oracle.exe
```

Both GitHub-hosted Windows x64 clang-cl jobs compile and execute the standalone test. The CMake job additionally registers `texture_gc_policy_oracle` in CTest, builds complete `kyty_emulator` and runs the existing headless CLI smoke test. Independent research assertions stay enabled in Release. The oracle explicitly prints `production_gc=NO_GO`.

**G2-B0 ACCEPT** requires all independent oracle checks and negative mutants, both hosted Windows jobs, full CMake/CTest, complete emulator and no telemetry CSV without opt-in. **Production GC policy NO-GO** continues until representative game traces, GPU/ETW/driver resource pressure, actual writeback/aliasing correctness tests and Vulkan integration prove a change justified. None of the synthetic thresholds should be copied into the renderer.

Vulkan lifetime constraints: [vkDestroyImageView](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyImageView.html) and [vkDestroyImage](https://docs.vulkan.org/spec/latest/chapters/resources.html) require completion of submitted resource users before destruction. The CPU model only approximates this through a monotone fence and does not simulate host synchronization.
