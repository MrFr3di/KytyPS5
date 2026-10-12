# KYTY-INDEX-001 S4-B5-D — Short-path dispatch & Windows codegen

**Only branch:** `MrFr3di/KytyPS5:perf/texture-range-index`. **Journal:** Mathlab issue #289. Fork `main`, upstream, Mathlab source, PRs and merges are prohibited. **Production source and existing S4-B5-A/B/C benchmarks are unchanged by this slice**. `KYTY_TEXTURE_CANONICAL_BUCKET_SKIP=OFF` remains the default.

## Motivation and falsifiable hypotheses

S4-B5-C delivered reproducible synthetic long-range CPU lookup wins for T256 and short-range Windows regressions even when T256 selected the same legacy Find loop. This does not prove that a branch alone causes them: the duplicated compiled template can alter optimizer decisions, layout, inlining, register pressure, instruction-cache effects, scheduling and noisy runner latency.

H-D1 (branch overhead): there is a stable positive cost for `ForEachPageCountCanonicalOwnerPage<256>` on short false-path queries relative to the exact `ForEachCanonicalOwnerPage<false>` helper. H-D2 (codegen): moving the same condition into the calling function changes static instruction/layout features; no cycle-level claim can be based on assembly alone. H-D3 (prior noise): the short regression attenuates or changes sign with repeated matched measurements. None is assumed true.

## Fixed CPU comparison

Six modes compiled into a **separate** `ShortPathCodegenBench.cpp`:

1. `direct_page_find`: explicit scalar `pages.Find(page)` from the caller
2. `canonical_helper_legacy`: existing helper, compile-time `false`, reference
3. `page_only_t256`: existing opt-in helper with runtime page count
4. `manual_page_t256`: exactly the same predicate placed at the caller
5. `page_only_t1024`: high-threshold false-branch control
6. `always_bucket_skip`: existing unconditional skip

Only canonical `MultiLevelPageTable<InlinePageOwnerList<Common::SlotId,16>,20,44,14>`, real `SlotVector`, correct `query_epoch` and effective-overlap filtering. The independent page-major oracle replays registration footprints and current effective images, validates order, generations, churn, invalid 44-bit range and retained empty allocated L1. No sidecar, heap metadata, allocation policy or TextureCache data change.

Six fixed scenarios: dense overlap, 44-bit boundary, short L1 crossing, short single L1, long single L1, medium single L1. Frozen warm batches are `[8, 512, 2048, 2048, 128, 128]` for 96 query templates before and after churn. **Six scenarios × six modes × five separate processes = 180 independent records per host**. Rotate mode order between repetitions; CSV includes registration/churn/query ns, native checksums, visited owners, allocated L1 counts, operator-new requests and selected mode counts. Use raw 5-repeat paired ratios, medians, **minimum paired ratio**, and absolute ns/query; neither a 5-repeat minimum nor medians confer statistical significance. CPU frequency, system load and function placement are unpinned on hosted runners.

## Codegen probe

Separate `ShortPathCodegenProbe.cpp` contains four named `extern "C"` **noinline** functions: direct legacy, helper legacy, T256 helper, manual T256. These are a *minimal canonical-owner loop*, NOT the compiled production `TextureCache::FindImagesInRegion`; callbacks sum native-ID values but do not call image-overlap or epoch filtering. On Windows clang-cl fast compile the real optimized COFF `.obj`, disassemble using `llvm-objdump` or `dumpbin` from hosted toolchain, and retain raw object/disassembly plus SHA-256 provenance CSV. The parser only verifies symbol presence. Static disassembly is a diagnostic; **not** calibrated instruction/µop/cycle counts or causal FPS proof.

## CI and criteria

Use the existing three GitHub-hosted jobs: Linux GCC CPU portability + native oracle, Windows clang-cl fast + COFF disassembly, and full Windows CMake/CTest/full opt-in emulator+CLI. Retain old S4-B5-C artifacts separately. No extra workflow and no self-hosted runners. Each runner must PASS Python negatives, native oracle and all raw-row/heap parity checks. Compare deterministic native checksums, owner visits, bucket and dispatch counts across hosts; nanoseconds are never cross-host parity fields.

RESEARCH ACCEPT: complete hosted CI, reproducible native parity, matched raw CSV with documented variances and authentic Windows disassembly evidence. Missing disassembler or failed codegen parsing means blocked provenance, not a made-up assembly result. A measured short-path loss may still be a legitimate RESEARCH ACCEPT, but **performance/production GO remains NO-GO** without representative real game lookup workload, Vulkan activity and end-to-end frame-time/FPS validation.

Next possible slice after evidence: if the short-path regression is stable, isolate code layout and quantized branch costs before considering a minimal production change; otherwise prioritize representative workload frequencies rather than speculative complexity.
