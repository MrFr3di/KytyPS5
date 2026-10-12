# KYTY-INDEX-001 / S4-B5-C — canonical page-count-only policy

**Experimental status:** CMake `KYTY_TEXTURE_CANONICAL_BUCKET_SKIP=OFF` by default; ON now compiles the S4-B5-C page-count-only T=256 candidate. This is CPU research; NO in-game Vulkan, FPS or production policy approval.

## Research question and comparator

S4-B5-B showed a meaningful long-query win with unconditional canonical L1 bucket skipping, **even inside one already-allocated L1 bucket**; its crossing-L1 adaptive T256 necessarily missed that case. Why? The bulk walk performs one bucket lookup followed by contiguous page access, whereas legacy `Find(page)` repeats page→L1→L2 decoding. This is a plausible mechanistic explanation, not a production theorem or a confirmed branch-cost profile. Synthetic Windows full also had short range regressions, where B5-B measured only 96 queries/phase.

Freeze six modes: (0) legacy, (1) unconditional allocated-bucket skip, (2) crossing-L1 AND >=256 pages (B5-B comparator), (3) page-only >=64 pages, (4) page-only >=256 pages (compiled opt-in research candidate), (5) page-only >=1024 pages. No change to prior B5-B benchmark. Use exact canonical `MultiLevelPageTable<InlinePageOwnerList<Common::SlotId,16>,20,44,14>` and `Common::SlotVector`.

## Dispatch, bounds, semantic proof

`ShouldSkipCanonicalBucketsByPageCount<T>` returns true only if

```
first < last_exclusive &&
last_exclusive <= kPageCount &&
(last_exclusive - first) >= T
```

All calculations use valid `size_t` endpoints, with short-circuit validation preceding subtraction. A zero, inverted, overflowing byte region or 44-bit out-of-range query is rejected via unchanged production `TryGetPageRange`. Both branches call the S4-B5-A canonical helper. The skip path only skips *never-allocated* L1 buckets and traverses entries within present buckets in ascending page order. Therefore page-owner enumeration is exactly equal to legacy for all valid queries, independent of T and whether any L1 was once allocated and emptied. Existing `query_epoch` and live `Image::Overlaps` receive identical owner events. No second index, heap state, page mutation, registration or GC modifications.

## Frozen hosted experiment and anti-noise changes

S4-B5-C supplies a new standalone `PageCountCanonicalPolicyBench.cpp`, independent native ordered oracle and regression/negative Python tests. Six B5-B scenarios remain unchanged; seventh `mid_single_bucket_64` checks a 64-MiB range that stays within one allocated L1. Each scenario has 96 query templates before and after churn, generation-reuse checks and live effective ranges. Mode order is alternated across three fresh-process repetitions: **7 × 6 × 3 = 126 subprocesses per GitHub-hosted runner**.

To reduce timer quantization for short lookups, identical queries are repeated in **warm synthetic batches**, fixed before experiments at `[8, 4, 8, 128, 256, 128, 128]` according to scenario order (broad 8-GiB scans must not be repeated excessively). Each subprocess preflights the independent page-major native oracle before timing, then measures registration, batched pre-churn lookup, churn, and batched post-churn lookup separately. Preserve first-observed ordered `SlotId` checksum, allocated canonical bucket counts, visited owners, number of `operator new` requests and selected dispatch counts. Note that this is an intentionally warm replay; cache-miss/cold and actual game workload characteristics are NOT represented.

Python driver checks matched groups (all six policies same semantic and allocation output), frozen batches, every query counted as skip or find for adaptive modes, required true and false dispatch coverage, and deterministic checksum across repeats. Summary includes median query ns per lookup, both ratio of medians and **worst of three paired ratios** for short regressions. Timing ratios are compiler and runner-dependent; never compare absolute nanoseconds across different OS/toolchain as parity.

CI stays within the same three GitHub-hosted jobs: Linux GCC standalone only, Windows clang-cl fast, Windows full CMake/CTest plus complete `kyty_emulator` build with opt-in ON and headless CLI. Separate CSV/driver artifacts for B5-C. The previous B5-A/B5-B benchmarks and artifacts are preserved. Do not add another GitHub workflow.

## GO/NO-GO

RESEARCH/COMPILE ACCEPT requires all source-bound oracle/negative checks, native generation/order/visit/allocation parity, cross-platform stable cells and exact-head full Windows compile+CLI. A tentative performance recommendation requires a reproducible Windows full benefit for long single-bucket queries **and** no material regression on short 44-bit boundaries after batching. Choosing an optimum based on Linux or a single synthetic workload is forbidden. `KYTY_TEXTURE_CANONICAL_BUCKET_SKIP` stays **OFF** in default builds; production integration/default ON is **NO-GO** until representative game/Vulkan usage, meaningful measured lookup distribution and end-to-end FPS/frame-time evidence.
