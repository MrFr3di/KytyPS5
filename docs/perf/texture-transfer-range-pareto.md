# KYTY-INDEX-001 G1-B1 — Real RangeSet / transfer-plan Pareto study

Research log: [Mathlab #289](https://github.com/definitely-stable/Mathlab/issues/289). Only code branch: `MrFr3di/KytyPS5:perf/texture-range-index`. Validation only on GitHub-hosted Windows Server 2022 x64 clang-cl. No owner PC, games, or physical GPU needed.

## Source audit and research question

`BufferCache::DownloadBufferMemory` (actual `src/graphics/host_gpu/renderer/cache/bufferCache.cpp`) intersects GPU-dirty ranges with `RangeSet::ForEachInRange`, issues one `vk::BufferCopy` per disjoint selected dirty run, and packs staging offsets by `Common::AlignUp(end-start,64)`. This is *not* GPU physical-byte telemetry: alignment pads staging offsets and does not automatically imply more bytes read from the source Vulkan buffer. Existing G1-A aggregate counters cannot tell whether dirty fragmentation causes material copy-command or staging-space overhead.

`tests/TransferRangeParetoTests.cpp` compiles the **real production** `src/graphics/host_gpu/rangeSet.h` (not a copied range-set implementation) and uses it to generate actual disjoint source ranges. Independent 512-byte byte maps track deterministic randomized `Add`, `Subtract`, clipped `ForEachInRange` semantics. It models the audited baseline readback descriptor sequence and counterfactual gap bridging with thresholds 0/16/32/64/128 bytes and an aggregate 64-byte overfetch limit. This is a **planning-only simulation**: bridging otherwise clean gaps has *not* been proven safe for GPU ownership, host readback, dirty state, coherency or Vulkan barriers.

Each plan is checked for all requested dirty-byte coverage, monotone/non-overlapping copy extents, in-bounds copy regions, 64-byte staging starts, nonoverlapping packed staging and exact clean-gap overfetch accounting. Four intentionally unsafe mutants (dropped dirty copy, overlapping staging, hidden clean overfetch and out-of-range readback) must all be rejected. A distinct source contract Python check fails on altered production copy-offset/64-byte packing behavior, and has three negative mutations; it is a text tripwire, not a C++ proof.

## Interpretation and decision

The built-in 8+8 bytes / 8-byte-gap fixture demonstrates a genuine **trade-off**, not a universal speedup: baseline two copies, 16 GPU copy-payload bytes and 128 bytes staging footprint, versus one hypothetical merged copy, 24 GPU copy-payload bytes and 64 bytes staging. Reducing command count can increase GPU copy bytes. Increasing the gap further need not improve the trade-off.

The emitted `transfer-range-pareto[-fast].csv` contains baseline and each counterfactual's dirty-byte count, **modeled** copy payload, staging bytes, copy-region count, overfetch and fixed-seed trial count. These are synthetic planner metrics, *not* GPU timings, measured PCIe bytes, Vulkan driver cost or frame-time estimates. Their decision marker is `MODEL_ONLY`.

## Acceptance and restrictions

Both feature-branch `windows-2022` jobs must run this C++20 test, the Python source tripwire and existing tests. The full job must retain CMake/CTest, entire `kyty_emulator` build and headless CLI smoke; CSV artifacts are attached to exact HEAD. G1-B1 can be ACCEPT as a source-anchored transfer-plan experiment only when both jobs are SUCCESS. Production gap merging stays **NO-GO**: a separate shader/buffer ownership proof, real GPU workload evidence and barrier analysis would be required. No production Vulkan/TextureCache/BufferCache/scheduler, GC or descriptor behavior changes.
