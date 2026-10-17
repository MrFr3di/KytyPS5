# CPU-PROFILE-001C — Profiling overhead calibration, Windows + Linux

## Decision

CPU-PROFILE-001A and 001B established the correctness of opt-in
`Common::DebugCounters` and five native SyncOnAddress guest-HLE source
families on GitHub-hosted Windows x64, but did not establish *measurement
overhead*. Profile-on vs profile-off cost must be measured before using
instrumented wall-time samples to prioritize a real-game CPU change.

**Research-only branch:** `perf/cpu-runtime-research`. Fork main, upstream
and the previous texture branch remain untouched. Both GitHub-hosted jobs
run on the **same commit** under the same workflow:
- Windows 2022: `clang-cl`, production portable `CondVar` path.
- Ubuntu 24.04: `clang++`, production Linux `futex` path.
No self-hosted runners, local GPU test claims or unrelated changes.

## Experiment 1 — diagnostic overhead

`tests/SyncOnAddressCpuBench.cpp` retains the original four cases,
nine rounds of timing and precise return-code assertions, with two optional
modes:
- `--profile-off`: disable all debug counters before timed measurements.
- `--profile-on`: enable identical native instrumentation before measurements.
- No switch means OFF, so original frozen baseline A-B-B-A remains comparable.

Execute the same **candidate binary** four times on each host:
OFF, ON, ON, OFF. Each process warms every case and emits strict CSV:
`case,iterations,rounds,min_ns,median_ns,max_ns`.
Save all four raw files plus the analyzer's output and runner/compiler identity.

`scripts/analyze_cpu_profile_overhead.py` validates every raw CSV with the
strict CPU-SYNC-001 parser, checks identical iteration counts, and emits:

- per-case OFF and ON nanoseconds per operation (median of two process medians);
- `on_over_off_pct = 100 * (ON/OFF - 1)`;
- between-process drift for both OFF and ON;
- within-process min/max spread for both OFF and ON;
- `NOISY_SYNTHETIC` if **any** of those four spreads is greater than 20%;
  otherwise `SYNTHETIC_ONLY`.

The marker `CPU_PROFILE_001C_ANALYZER_PASS` means **only that all files
parsed successfully**, never that instrumentation has zero overhead, that
ON is faster, or that an FPS target was attained. A cloud runner is noisy;
with two process medians per mode, there are no justified confidence
intervals or proof of statistical significance. This is a calibration,
not a statistically powered field benchmark.

The microbenchmark's zero-timeout cases include the normal path's full
registry/futex overhead where applicable. The mismatch fast paths can be
only a few nanoseconds and are susceptible to timer resolution, compiler
inlining/optimization, and DVFS, so unusually high ratios should be treated
as a diagnostic flag rather than an estimated game speedup.

## Experiment 2 — cross-platform correctness

Both jobs build from the real production kernel translation units:
`syncOnAddress.cpp`, `eventFlag.cpp`, `semaphore.cpp`.
Before CMake configure each job extracts the **same immutable baseline
source** (`b82512be065b2fa2432cfa4e5a8c779604236299`) into its own
build tree. Both platform jobs then require:

1. Python unit tests for the old and new strict CSV analyzers.
2. Candidate and baseline SyncOnAddress CTests, plus EventFlag, Semaphore,
   event queue and HLE-classifier CTests.
3. `ctest --repeat until-fail:10` for the semaphore lifecycle oracle.
4. Existing A-B-B-A original-baseline vs candidate synthetic measurements
   (separate from ON/OFF diagnostic calibration).
5. Complete headless emulator compilation and headless CLI smoke.
6. Raw artifacts; both jobs have timeout and fail on missing tests.

Linux uses the package list from fork main's existing ubuntu-24.04
workflow (except Qt, since the CPU workflow explicitly disables the launcher).
The Linux glslang compiler and ALSA/DBus/Wayland/X11 dev libraries are still
needed by the emulator's dependency graph. No Windows binaries or Windows
libraries are used in the Linux job.

**Important cross-OS comparison:** Windows portable address-wait and Linux
futex use different production implementations. Their latency numbers
must **not** be compared as a claimed Windows-to-Linux relative speedup.
Use independent platform baselines and separate within-run profile ON/OFF
ratios. The original source's Linux futex path already has a direct
value-mismatch fast check; the Windows change targeted the portable path.

## Interpretations and nonclaims

- `CpuAddressRegistered` counts registration attempts and can include
  matching zero-timeout paths that never park.
- `CpuAddressParkCalls` counts *completed* wait-primitive invocations;
  not every futex or CondVar attempt actually triggers a context switch.
- All measured wait durations are elapsed wall time, which overlaps across
  guest threads and is **not CPU utilization**.
- Real-game FPS, frame-time percentiles, game load, cache contention and
  CPU-worker attribution are not measured by this CI.
- The new flag switches can change the benchmark's environment, including
  counter atomic operations, timing calls and cache footprints. This is
  precisely the overhead being investigated, not a pure clock-source
  baseline.
- Linux/Windows correctness success is not a mathematical race-freedom proof.

## Acceptance

`CPU-PROFILE-001C=VALIDATED` requires Windows **and** Linux hosted
workflows both SUCCESS on identical feature-branch SHA, strict analyzers
PASS, valid OFF/ON/ON/OFF artifacts and full emulator smoke. Keep separate:
`INSTRUMENTATION_CORRECTNESS` vs `NO_GAME_PERFORMANCE_VERDICT`.
If Linux discovers real differences in native futex/guest-signal behavior,
treat those as independent correctness findings and fix with evidence,
rather than ignoring Linux job failures.
