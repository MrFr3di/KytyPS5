# KYTY-INDEX-001 — Opt-in TextureCache range telemetry

Research tracker: [Mathlab #289](https://github.com/definitely-stable/Mathlab/issues/289).
Work branch: `perf/texture-range-index` only. The production page-table
lookup is **not replaced** by H0/H1.

## Why this exists

The isolated CPU experiments show that H1 can reduce work on large, sparse
ranges but regress during frequent unregister/register churn. Synthetic
suppression of warm descriptor calls is **not** a measurement of real
descriptor caching or actual renderer workload. This diagnostic collects
the workload *that reaches* `TextureCache::FindImagesInRegion`, together
with the lifetime of each registration generation. It does not count
descriptor requests intercepted earlier by a descriptor cache, and must not
be used to claim a true descriptor-cache hit ratio.

## Activation — Windows only

Two independent switches must be enabled:

1. **Build-time:** CMake `-DKYTY_TEXTURE_RANGE_TELEMETRY=ON` for
   `kyty_emulator`. The option defaults to **OFF**, leaving the ordinary
   build with no instrumentation and no telemetry fields.
2. **Runtime, before launching the emulator:** define the exact environment
   variable `KYTY_TEXTURE_RANGE_TRACE=1`.

PowerShell example, using an already configured Windows build:

```powershell
cmake -S . -B _Build/diagnostic -G Ninja -DKYTY_BUILD_LAUNCHER=OFF -DKYTY_TEXTURE_RANGE_TELEMETRY=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build _Build/diagnostic --target kyty_emulator --parallel 3
$env:KYTY_TEXTURE_RANGE_TRACE = '1'
# Run the emulator as usual; close it normally to flush the CSV.
# The file is written into the emulator process's current working directory.
Remove-Item Env:KYTY_TEXTURE_RANGE_TRACE
```

The build command assumes the normal Kyty Windows dependencies and a correctly
initialized MSVC/clang-cl build environment. The CI workflow verifies compilation
of the **enabled binary** but deliberately does not start a PS5 game or collect
a supposed real-game trace.

## Output and privacy

On normal `TextureCache` destruction, the instrumented executable writes
`texture-range-telemetry.csv` to its **current working directory**.
The file is replaced on each run. It is *not* emitted if the environment
switch is absent or unequal to `1`. A process crash may prevent the report
from being written. Ensure that the working directory is writable.

The report contains exactly two columns (`metric,value`) and aggregate
counts only, no guest memory addresses, IDs, file names, image formats,
shaders, descriptor identities or per-query histories. This supports sharing
aggregated performance evidence with the research issue without uploading a
game memory trace.

Key fields:

| Metric | Meaning |
|---|---|
| `queries` / `invalid_queries` | Actual lookup calls reaching the index and rejected ranges |
| `query_pages_1`, `2_3`, `4_7`, `8_15`, `16_63`, `64_255`, `256_plus` | Histogram of 1-MiB page footprints of valid queries |
| `query_ns_pages_*` | Inclusive `steady_clock` nanoseconds in lookup for corresponding bins, not per-query p95 |
| `owners_inspected` | Owner-list entries visited, including duplicates before epoch deduplication |
| `results` | Accepted unique owners emitted by legacy lookup |
| `page_overlap_queries` | Calls requesting 4-KiB page-overlap filtering |
| `registrations` / `unregistrations` | Successful registration lifecycle operations |
| `registered_pages` / `unregistered_pages` | Sum of coarse page membership work |
| `completed_generations` | Generation closed by register/unregister, includes startup empty generations |
| `generations_with_queries` | Completed generations containing at least one valid lookup |
| `generations_with_eight_large_queries` | Completed generations with at least eight lookups touching at least eight coarse pages |
| `max_large_queries_per_completed_generation` | Maximum such lookups between two changes |
| `current_generation_queries` / `current_generation_large_queries` | Open generation at time of report, intentionally not folded into completed histogram |
| `max_active_images` | Highest active registered-image count observed |

`queries` is the number of *range index* queries, not all descriptor lookup
attempts or a GPU frame counter. This telemetry is a diagnostic build and
adds clock reads and accounting to the measured path; any baseline vs H1
performance benchmark must be re-run under matched instrumentation settings.

## Decision protocol

Capture multiple representative gameplay segments and document separate
session conditions: title, renderer workload phase, amount of time, and whether
memory churn was expected. A minimum of three repeated sessions per workload
is a useful initial check, not a mathematical sampling guarantee.

Compare query-page histograms, large-query density per unchanged generation
and registration churn. If the majority of generations cannot amortize
snapshot construction, H1 is **NO-GO** regardless of synthetic speedups.
Do not infer cold/warm hit rates from this CSV; add explicit descriptor-cache
counters at the source of memoization if this later becomes necessary.

All evidence and GO/NO-GO decisions go into Mathlab #289. Do not merge
the feature branch to `main` as part of this research protocol.
