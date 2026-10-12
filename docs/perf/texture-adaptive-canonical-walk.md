# KYTY-INDEX-001 / S4-B5-B: adaptive canonical L1 bucket skipping

**Status:** experimental; OFF by default; no gameplay/FPS measurement.  
**Change scope:** only MrFr3di/KytyPS5 `perf/texture-range-index`; process log in Mathlab #289. No fork `main`, upstream, PR, or Mathlab source modification.

## Decision rule

The existing canonical `MultiLevelPageTable<InlinePageOwnerList<ImageId,16>,20,44,14>` has 1-MiB pages and 1,024 pages per allocated L1 bucket. A query first uses `ImagePageTable::TryGetPageRange`, which rejects zero lengths, overflow and out-of-address-space ranges. The adaptive query selects the pre-existing `ForEachAllocatedPageInRange` only if both conditions hold:

1. `last_exclusive - first >= MinimumPages`.
2. `first / 1024 != (last_exclusive - 1) / 1024`.

Otherwise it invokes exactly the former `table.Find(page)` loop. The terminal subtraction is valid after range checking. Both paths call the same owner callback, in ascending canonical page order, ignoring empty owners. No density counters, new memory allocation, second range index, sidecar, mutation to storage or reordering.

The frozen research sweep comprises thresholds `16`, `64`, `256`, `1024` pages. The currently compiled ON path uses **256** as a provisional policy to exercise the full emulator compile; this is **not** a demonstrated optimum. The legacy CMake option `KYTY_TEXTURE_CANONICAL_BUCKET_SKIP` is still **OFF by default**. ON now means opt-in **adaptive** dispatch, rather than unconditional bucket skip. The real `TextureCache::FindImagesInRegion` keeps `query_epoch`, native `SlotId`, live `Image::Overlaps`, and identical telemetry queried-page accounting.

## Falsifiable evidence

`tests/AdaptiveCanonicalBucketBench.cpp` executes real `SlotVector<...>`, `SlotId`, `MultiLevelPageTable`, canonical inline owner lists and the actual helper. The independent oracle enumerates frozen registered pages and current effective image ranges, not candidate index data. It checks exact output order and native generations before and after churn, plus invalid 44-bit ranges and an allocated-then-emptied L1 bucket.

Six workloads: dense overlap, sparse multibucket, bucket churn, 44-bit boundary, short L1 crossing, and long within-one-bucket. Six modes: legacy, unconditional skip, adaptive T=16/64/256/1024. Three independent subprocess repeats per mode/workload with alternating order: **108 processes**; every run has an independent preflight oracle. Report registration, lookup before/after churn, mutation time, C++ allocation requests, canonical allocated bucket count, visited owners, checksum and adaptive-mode selection counts. Each matching 6-mode group must agree on native checksum, owner visits, canonical buckets and new requests. Runner-specific nanoseconds are not exact stable cross-host facts. Tag CSV `SYNTHETIC_CPU_ONLY`.

Python driver tests reject wrong mode sets, unstable checksums/SlotId generation, owner visits, allocation parity mismatches and incorrect adaptive path counts. Source contracts reject removal of production fallback, threshold, canonical page bound, true native oracle, OFF default, and Windows compile flag.

Existing three workflows: Linux GCC CPU only, Windows clang-cl fast and full Windows CMake/CTest plus full `kyty_emulator` and headless CLI. Preserve original S4-B5-A CSV and benchmark unmodified; produce separate S4-B5-B raw/summary/driver artifacts.

## Mathematical invariants and risk

The skip branch can suppress only never-allocated buckets, which have no owners. For every allocated bucket it inspects the same pages in ascending order as the legacy walk; filtering nonempty owners is identical. Therefore for a valid query both paths enumerate the same owner lists in the same sequence, with the same `query_epoch` and effective-overlap filtering. This proof does not depend on threshold T or whether formerly allocated buckets are currently empty.

Potential slowdown remains in multi-bucket dense ranges and under runner noise; the policy also cannot skip empty pages inside a formerly allocated bucket. No guaranteed optimization win, performance SLA, RSS benefit or PS5 FPS effect can be derived from synthetic CPU runs. Evaluate *worst regression*, not only sparse speedup. Full Windows compile success proves integration only, not graphics/runtime correctness. Production default remains OFF without real game trace frequency and in-game verification.
