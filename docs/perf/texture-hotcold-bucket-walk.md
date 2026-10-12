# KYTY-INDEX-001 S4-B5-E — Hot legacy / cold bucket walk

**Protocol and evidence journal:** [Mathlab issue #289](https://github.com/definitely-stable/Mathlab/issues/289). **Only permitted code branch:** `MrFr3di/KytyPS5:perf/texture-range-index`. Never modify fork `main`, upstream, Mathlab repository source, open PR or merge. All CI jobs are existing GitHub-hosted runners.

## Research rationale

Prior S4-B5-D (success on GitHub-hosted CPU and complete Windows emulator compilation) observed reproducible 10–40% false-path regressions of the PageCountOnly T256 dispatcher versus the canonical legacy helper in warm Windows synthetic workloads. An isolated Windows clang-cl -O2 COFF probe reported 417 static code bytes for helper legacy and ~1,121 bytes for inline T256. That observation neither proves L1I cache misses nor establishes the actual production codegen.

H-E1: keeping a legacy `ForEachCanonicalOwnerPage<false>` walk inline while calling only the less-frequent `ForEachCanonicalOwnerPage<true>` arm through a `NOINLINE` function reduces the hot function static size and improves short-query throughput. H-E2: moving the *entire* conditional dispatcher out of line incurs call overhead even on the false path. Both are hypotheses with explicit NO-GO if contradicted.

## Benchmark and oracle

**No production .cpp/.h edits** in S4-B5-E. `TextureCache::FindImagesInRegion` still uses S4-B5-C PageCountOnly T256 only when CMake `KYTY_TEXTURE_CANONICAL_BUCKET_SKIP=ON`. Default remains **OFF**. No new index or extra state.

New `tests/HotColdCanonicalBench.cpp` compiles six genuine native-key policies, in frozen CSV order: canonical helper legacy; existing fully inline T256; inline legacy + NOINLINE skip only; NOINLINE entire dispatcher; manual T256 in caller; unconditional allocated-bucket skip. All share existing `MultiLevelPageTable<InlinePageOwnerList<Common::SlotId,16>,20,44,14>`, `SlotVector`, query-epoch dedup, logical image overlap and effective-range mutations.

The six existing B5-D workload traces remain fixed: dense overlap, near 44-bit limit, short L1 crossing, short single bucket, 256-MiB long single bucket, 64-MiB medium single bucket. Warm repeat factors `[8,512,2048,2048,128,128]` apply to 96 deterministic query templates both before and after native generation-reuse churn. Each process validates all six variants against an **independent page-major ordered oracle** before any measurement, tests invalid intervals and retained now-empty allocated L1. Mode order alternates across five independent repetitions. Thus **6 scenarios × 6 policies × 5 runs = 180 isolated processes per host**. Summary retains paired worst ratios, absolute ns/query, registration/churn ns, operator-new requests (not RSS or GPU allocations), dispatch choice and checksum. The workload is CPU-only warm replay, not FPS, p95 game latency or cold-cache measurement.

## Real Windows x64 codegen evidence

Separate `tests/HotColdCodegenProbe.cpp` compiles five exposed `extern "C"` NOINLINE symbols: `KytyB5ELegacy`, `KytyB5EInline256`, `KytyB5EColdSkip`, `KytyB5EHotCold256`, `KytyB5EFullDispatcher256`. The COFF object and optimized `llvm-objdump` disassembly are retained as CI artifacts. The parser checks exact symbol provenance and, where the disassembly format allows, static instructions/bytes and SHA-256. Hot/cold may have a smaller *wrapper* but total cold symbol size must be reported separately. Neither branch counts nor static bytes prove CPU cycles, and the probe is NOT actual production `TextureCache` with image-overlap or epoch.

## Acceptance / decision

All old CPU benchmarks stay frozen. The same three GitHub-hosted CI jobs run this new set: Ubuntu GCC CPU; Windows fast clang-cl with real COFF; full Windows CMake/CTest + entire ON-compiled emulator + CLI smoke. Research accept requires complete exact-head CI, 180 rows / 30 groups in **each** artifact, cross-host stable native checksum+dispatch parity, no extra `operator new` calls in matched groups, and verifiable COFF provenance.

A favorable default-on performance recommendation is **not authorized** in this slice. Short-range throughput must first improve without a material long-range regression on Windows full (plus paired-repeat caveats); representative real-world workload distribution, Vulkan/GPU and frame-time/FPS remain mandatory before production integration.

If the noinline cold arm still regresses short queries, stop purely synthetic threshold tweaking and prioritize finding actual texture lookup-length frequencies in a controlled production opt-in telemetry experiment on GitHub-hosted Windows, without corrupting frame timing.
