# KYTY-INDEX-001 G3-D0 — source-anchored DescriptorHeap capacity research

Research issue: https://github.com/definitely-stable/Mathlab/issues/289. Code only in `MrFr3di/KytyPS5:perf/texture-range-index`. No production source changed.

Actual `DescriptorHeap::Commit` uses per-layout batches of 32, descriptor pools max 1024 sets, per-type limits 8192 storage-buffer, 8192 sampled-image, 1024 storage-image and 1024 sampler descriptors. On out-of-pool/fragmentation `Allocate` halves the request. Pool rollover is GPU-tick-gated by `CurrentTick/IsFree` with `m_sets.clear()`. CommitBindings differs for push descriptors versus update/bind descriptor heap path.

`DescriptorHeapCapacityOracleTests.cpp` models bounded capacities, layout mix, per-batch set uniqueness, deferred-pool generations and pool ledger over 1536 commits x 4 synthetic workload distributions x batch sizes 1,8,16,32. Negative checks reject capacity overflow and early-reset conditions. Source-contract Python guards source constants, halving, layout invalidation, pending pool safety and both bindings paths; 7 intentionally broken source variants must be rejected. Real `DescriptorHeap` C++ and Vulkan are NOT executed. No GPU timings, driver fragmentation, FPS, workload traces or real memory usage are measured. These data cannot authorize a production batch-size change.

Parallel GitHub-hosted Ubuntu 24.04 GCC C++20 CPU lane supplements (never replaces) both existing Windows 2022 clang-cl jobs; Linux runs only independent CPU experiments and source checks, produces separate SHA-tied CSV artifacts, and does not build emulator. G3-D0 is accepted only on exact same HEAD with all three jobs SUCCESS, full Windows emulator build/CLI smoke preserved, `G3_MODEL_ONLY,PASS` present. All production optimization remains NO-GO pending representative workload and driver evidence.

## G3-D1 — cumulative descriptor-set discard and cold-start allocation

The previous G3-D0 `stranded` CSV field measured only **spares still held at the end of the final pool**. It did **not** count spares from earlier pools whose per-layout batches were erased by the audited `m_sets.clear()` on each rollover. Treating the final number as total waste was therefore incorrect.

The same C++20 executable now supports `--waste`. Before every pool rollover it accounts for all old layout batches' cached **unissued** sets as cumulative `discarded_spares_on_rollover`. Independently at every commit and at test completion it asserts:

```
allocated_sets = delivered_commits + discarded_spares_on_rollover + current_cached_spares
```

The proof obligation is descriptor-*set* capacity, not bytes or GPU time; sets handed to a caller remain allocated until pool reset and are not counted as spares. Separate ledger checks validate four per-type descriptor budgets, set serial uniqueness, generation ownership and tick-gated pool recycling. The fixed-seed adversarial source-independent fixture forces a cold layout to leave unused sets when a different layout exhausts the storage-image budget. Two conservation mutants (erased cumulative discard and double-counted spare) must be rejected, in addition to source tripwires and rollover checks.

The 5 synthetically generated scenario families and 5 policy candidates produce 25 rows of identical 1,536-commit workloads each: fixed batch 1, 8, 16, 32 and counterfactual **cold8_warm32**. This last policy starts a layout at batch 8, changes to 32 only after that layout exhausts its first successful batch, and reverts to cold 8 whenever a new pool is created/reused. It is **not** present in Kyty production. Policies are compared by cumulative unused discarded sets, active spare sets, allocation attempts, halving retries, pool rollovers/new pools and peak pending pools; no model accepts a premature reset.

G3-D1 emits `descriptor-heap-waste-fast.csv` (Windows fast), `descriptor-heap-waste.csv` (Windows full), and `descriptor-heap-waste-linux.csv` (Ubuntu GCC) with the same deterministic set counts and explicit `G3_D1_MODEL_ONLY,PASS` marker. The old G3-D0 CSV and old CI checks remain intact. The same executable now has an additional `descriptor_heap_waste` CTest; no new CI job or standalone target. All three exact-HEAD hosted checks, full Windows emulator and CLI must pass for **CPU MODEL ACCEPT**. Driver allocation latency, Vulkan fragmentation, frame timings and application layout traces are absent. Therefore no production adaptive allocation or descriptor heap refactor is authorized.
