# KYTY-INDEX-001 / S4-B5-F — canonical range-shape evidence gate

**Implementation scope:** fork-only `MrFr3di/KytyPS5:perf/texture-range-index`. Journal in [Mathlab #289](https://github.com/definitely-stable/Mathlab/issues/289). Never edit fork `main`, upstream, Mathlab source or create/merge PRs.

## Why S4-B5-F is the next step

The Windows-hosted S4-B5-B/C/D/E experiments found conditional wins on long ranges, but repeatedly showed materially slower short/false-path lookups. They used synthetic, repeatedly warmed CPU replay. Threshold tuning is suspended until an authentic workload reveals **how many** real region-lookups are short, cross L1, or reach the proposed 256-page bucket optimization.

Kyty already had address-free opt-in `TextureRangeTelemetry` with seven page bins, query-time totals and epoch-generation counters, but lacked a way to separate crossing-L1 from within-one-L1 queries. Replacing it with per-call trace buffers or a new index would add overhead and confidentiality risks. S4-B5-F **appends** only the smallest aggregate counters needed to discriminate these policies.

## Instrumentation and switch semantics

Original build option is retained: `KYTY_TEXTURE_RANGE_TELEMETRY=OFF` by default. When explicitly compiled `ON`, the user must additionally set `KYTY_TEXTURE_RANGE_TRACE=1` for this instance of TextureCache to collect or write any CSV. This is separate from the experimental `KYTY_TEXTURE_CANONICAL_BUCKET_SKIP`, which remains `OFF` by default.

`TextureCache::FindImagesInRegion` continues to validate its existing `ImagePageTable::TryGetPageRange`, perform the original canonical owner walk, image overlap and query-epoch dedup. It invokes `OnQuery(...,elapsed_ns)` as before, then invokes `ObserveCanonicalSpan(first,lastExclusive,kBucketEntries,kPageCount)` **only after the existing measured interval has stopped**, and only when telemetry is compiled and runtime-enabled. An invalid/empty/oversized span is rejected before subtracting or calculating `lastExclusive-1`. No timestamps, bytes, guest page addresses, image descriptors, IDs, raw per-query data or dynamically allocated buffers are collected.

Append-only CSV counters (old CSV metric names and order kept):

| Counter | Meaning |
|---|---|
| `shape_queries` | Valid region lookups whose shape was observed |
| `shape_l1_cross_queries` | First and last page belong to different canonical L1 buckets |
| `shape_pages_64plus` | Valid lookups spanning at least 64 canonical 1-MiB pages |
| `shape_pages_256plus` | Valid lookups spanning at least 256 pages, T256 eligible |
| `shape_pages_1024plus` | Valid lookups spanning at least 1024 pages |
| `shape_pages_256plus_same_l1` | T256 eligible, both endpoints in the same L1 bucket |
| `shape_pages_256plus_cross_l1` | T256 eligible, L1-crossing |

All counters count observed region-lookup **calls**, not per-frame accesses, descriptor-cache hits, CPU cycles or distinct textures. The `queried_pages` sum and old seven-bin latency totals retain their original meaning. The measured `query_ns` includes existing opt-in clock/accounting, and is not uninstrumented gameplay latency. Calling an extra diagnostics function after the clock may still affect later calls and CPU caches; do not treat telemetry ON vs OFF differences as unbiased FPS evidence.

## Offline strict analysis

`scripts/analyze_texture_query_shape.py` accepts only existing complete `metric,value` telemetry CSV extended with all seven new shape counters. It validates both the old generation/7-bin accounting and: one shape per valid lookup, 64+ and 256+ equality with old histogram bins, 1024+ <= 256+ <= 64+ <= calls, exact split of T256 within/crossing L1, crossing-subset constraints, total queried-pages minimum and finite histogram upper bounds. Rejects missing, duplicate, negative or overflowing uint64 metrics.

Example for a real *user-provided* opt-in CSV, after consenting to run a game (not executed in hosted CI):

```text
python scripts/analyze_texture_query_shape.py texture-range-telemetry.csv
```

Output is `UNVERIFIED_SOURCE` and `BLOCKED_NO_REPRESENTATIVE_GAME_TRACE`. The command itself cannot verify the binary used, run duration, workload representativeness or real frame times. Real evidence requires a provenance manifest: emulator git SHA, Windows build configuration, both compile/runtime flags, GPU/driver, game executable identity/version (not private serial), input replay or repeatable workload description, number of frames and run duration, warmup period, frame-time distribution, and the raw aggregate CSV. Do not record file contents or guest addresses in telemetry.

### Synthetic CI gate

`TextureRangeTelemetryTests --shape-fixture` emits a deterministic aggregate fixture with seven edge-case spans (1,2,64,256,256,1024,1025 pages), both within-L1 and crossing-L1 cases and an invalid-bound rejection test in unit tests. `--fixture` labels offline output **SYNTHETIC_CI_FIXTURE** explicitly. This is NOT a real game trace. The three existing hosted jobs check the raw CSV and derived JSON, plus negative tests and source call-site contracts. Windows full also recompiles the complete `kyty_emulator` with `KYTY_TEXTURE_RANGE_TELEMETRY=ON`, but the headless user-name CLI keeps runtime telemetry OFF and must NOT emit a real telemetry CSV.

## Acceptance / next action

Research/compile ACCEPT: all three exact-SHA hosted jobs pass, real instrumented full Windows emulator compiles, headless CLI still produces no telemetry when runtime variable unset, all seven shape metrics and old metrics match independent fixtures/negative mutations, and outputs are strictly tagged synthetic.

**Actual game/production policy decision remains BLOCKED.** There is no representative game workload available through the current GitHub-hosted CI. Only a consented, reproducible Windows gameplay capture with independent frame-time measurement can establish real percentages or FPS impact. Do not keep re-optimizing synthetic thresholds or enable the bucket skip by default.
