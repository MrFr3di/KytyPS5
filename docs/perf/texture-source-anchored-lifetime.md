# KYTY-INDEX-001 G2-B2 — Source-anchored lifetime and scheduler contracts

Research log: [Mathlab #289](https://github.com/definitely-stable/Mathlab/issues/289).
Working branch ONLY: `MrFr3di/KytyPS5:perf/texture-range-index`. All validation on **GitHub-hosted Windows 2022 x64**. No user PC, PS5 game, GPU, ETW, Linux/macOS, or self-hosted runner.

## Why B0/B1 were insufficient

The finite-state oracles G2-B0 and G2-B1 model slot generations, per-object GPU fences and GC decisions but use separate abstract containers and scheduler transitions. They cannot show that the real Kyty `SlotVector` / LRU behave in the same way, or detect later source-code reordering inside actual `TextureCache::DeleteImage` / `CommandScheduler`. G2-B2 binds tests to actual implementation paths without enabling GC policy changes.

## Directly compiled production headers

`tests/SourceAnchoredTextureLifetimeTests.cpp` includes **the real production** `src/common/slotVector.h` and `src/common/lruCache.h` with the existing `tests/TextureRangeSupport/common/assert.h` shim used by earlier Windows-native range tests. Fatal validation is preserved; the test does **not** duplicate either container implementation.

It checks real destructor invocation at physical erase, unregistered-but-resident slots, no slot reuse while an erase is deferred, deque-stable addresses, occupied-size vs slot capacity, LIFO recycling and increasing `SlotId` generation, stale-ID rejection and identity hash, plus mutable and const `ForEach`. The reference LRU uses an independent vector history of insertion/touch ticks; a seeded deterministic sequence exercises real `LeastRecentlyUsedCache::Insert/Touch/Free/ForEachItemBelow` with varying age cutoffs and candidate budgets. The controlled fake deferred queue calls **real `SlotVector::erase`** after a synthetic completion tick. It cannot establish that native Vulkan scheduler callbacks always reach this state.

## Actual source contract checks

`tests/test_texture_gc_source_contract.py` reads the **checked-out real** `textureCache.cpp`, `commandScheduler.cpp`, and `renderContext.cpp`. Balanced-brace function extraction strips comments before checking semantic-order tripwires. The suite asserts:
- Depth/stencil associations are freed before root unregistration; GPU-modified guard and `UnregisterImage` precede deferred scheduling.
- The logical enqueue counter precedes the scheduler call; physical `m_slot_images.erase(id)` precedes deferred completion counter; inactive-scheduler erasure remains immediate.
- `UnregisterImage` removes LRU registration, updates accounted bytes and only then clears `registered`.
- `CommandScheduler::QueueOperation` captures `CurrentTick()`, drains through a safely unlocked inline fallback; `PopPendingOperations` requires GPU-fence readiness, waits priority dependencies before running normal callbacks.
- `CommandScheduler::Shutdown` waits the submitted tick, drains normal/priority queues and joins the priority thread before `Closed`; `RenderContext` destroys its scheduler before member `TextureCache`.

10 intentionally mutated source fixtures (plus actual-source and formatting-neutral positive controls) must be rejected. These are **textual source-change tripwires, not C++ AST or concurrency proofs**: they are deliberately strict, may require reviewed changes when legitimate source implementation changes, and can be bypassed by semantically equivalent unsafe source changes. They supplement but do not replace runtime tests. The directly compiled real containers are the primary executable evidence.

## GitHub-only acceptance

Both `windows-2022` x64 clang-cl jobs build/run `source_anchored_texture_lifetime_tests` and Python `test_texture_gc_source_contract.py`. Full job also registers CTest `source_anchored_texture_lifetime`, retains all earlier G0/G1/G2 tests and **full telemetry-enabled `kyty_emulator` build** followed by headless CLI smoke. No local commands are required. CI artifacts include `source-anchored-texture-lifetime-fast.txt` and `source-anchored-texture-lifetime.txt` with check counts, random seed fixed in source and `real_gpu_scheduler=NOT_EXECUTED`.

**G2-B2 source-anchored model ACCEPT** requires both exact-head Windows jobs SUCCESS, native container regression PASS, source-guard positive/negative tests PASS, CTest, full emulator build, and opt-in-off CLI smoke. Production dual-pressure eviction remains **NO-GO**: these tests do not execute `CommandScheduler` on a GPU, verify real image download/writeback, or establish gameplay FPS/VRAM improvements. Further work should target actual runtime integration that can run on GitHub-hosted CI without user PC.
