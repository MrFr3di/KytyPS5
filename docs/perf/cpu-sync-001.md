# KYTY-CPU-001 / CPU-SYNC-001 — Portable address-wait fast path

Status: **candidate implemented; CI verdict pending**.
Tracking: https://github.com/MrFr3di/KytyPS5/issues/1

## Repository safety / boundaries

Only `perf/cpu-runtime-research` is writable. Frozen original base:
`b82512be065b2fa2432cfa4e5a8c779604236299`.
Do not modify fork `main`, upstream, `perf/texture-range-index`, or target a PR at main.
Primary acceptance uses GitHub-hosted `windows-2022` clang-cl. Linux is
research-only for this candidate: its normal `WaitLinux` path already performs
an early value comparison and does **not** exercise `WaitPortable`.

## Mechanism and likely benefit

`WaitImpl` first validates the requested 32-/64-bit address, then calls
`WaitPortable` on Windows. The original portable implementation constructs
`PortableWaiter` (including a `Common::CondVar`), acquires a global registry
mutex, hashes the address, possibly allocates a shared registry entry, acquires
its mutex and adds/removes a linked-list waiter **before** concluding the word
does not match the expected value.

In the new candidate, an acquire atomic load returns `OK` before any waiter
registration if the word already differs from expected. The registered path
retains its existing recheck under the per-address mutex, timed wait and
unregister logic. The guest signal-safe-point callback remains in `WaitImpl`,
including for this short path and mismatched zero-timeout calls.

Conceptually, for mismatch `T_baseline = T_registry + T_waiter + T_lock +
T_compare + T_unregister`; `T_candidate ~= T_atomic_compare`. This is a
cost hypothesis, *not* a measured speedup. If real workloads spend little time
in mismatched waits, the system-wide win may be negligible.

## Race and correctness invariants

- Invalid or misaligned addresses must return `EINVAL`, without invoking
  guest signal callbacks (validation stays before `WaitPortable`).
- 32-bit and 64-bit atomic comparisons use acquire semantics.
- Both zero-timeout mismatch and unlimited-timeout mismatch return `OK`.
- Matching zero-timeout waits return `ETIMEDOUT`, with original semantics.
- A concurrent write may race between the preliminary load and registry
  acquisition. If the preliminary load sees a mismatch, immediate success is
  valid: the compare was genuinely unequal during the call. Otherwise the
  existing locked slow-path comparison prevents a missed registration/wake.
- Wake-one, wake-multiple, wake-all, zero-wake, address isolation, registration
  race, and 64-bit high-half comparison retain their established tests.
- Never bypass the `PollSignals()` at the `WaitImpl` boundary. The
  `Common::CondVar` callback set by `pthread.cpp` is part of guest signal
  delivery semantics. Replacing time-sliced waits with indefinite OS waits
  would require a separate cancellation/signal architecture audit.

## Correctness and reproducible performance

`tests/SyncOnAddressTests.cpp` covers registration races, mismatches,
wake-one/all and timeout behavior. The added repeated 32/64-bit mismatch
case also asserts one guest signal callback per valid `WaitImpl` call.

The standalone `tests/SyncOnAddressCpuBench.cpp` measures **real**
`SyncOnAddress` public functions linked separately against:

- frozen baseline: `git show b82512be...:src/kernel/syncOnAddress.cpp`;
- branch candidate: current `src/kernel/syncOnAddress.cpp`.

They share the same test driver and `common` library in the same
`windows-2022` job. Each process warms up then records nine iterations
(rounds) per case. A-B-B-A repeated process ordering mitigates thermal and VM
drift. Analyzer validates schema, sample counts, ranges and reports
baseline-over-candidate ratio and between-invocation drift. It flags >20%
between-run drift as `NOISY_SYNTHETIC`. Numerical speedup **cannot** override
a failed correctness gate.

Cases: `mismatch32`, `mismatch64`, `timeout32_zero`,
`timeout64_zero`; timeout cases are negative controls.

A successful synthetic benchmark does **not** prove lower frame time, better
frametime tail, more FPS, or any real-game improvement. Until game traces or
sampled workload evidence show frequent mismatched waits, production
performance verdict remains **NO_GAME_EVIDENCE**.

## CI

`.github/workflows/cpu-runtime-research.yml` runs only on CPU branch pushes
or explicit manual trigger. Hosted Windows 2022 clang-cl:
1. Extract immutable original source from frozen main SHA.
2. Configure CMake without Qt launcher.
3. Compile both production-source benchmark variants plus
   `sync_on_address_tests` and `event_queue_lifetime_tests`.
4. Run CTest correctness gate, then ABBA comparative measurements.
5. Build complete `kyty_emulator` and run headless CLI smoke test.
6. Upload raw CSV, summary and host hardware/toolchain evidence even on failure.

A green result must be observed on the exact candidate HEAD before accepting.
GitHub Actions queue/enablement and workflow discovery are independent
environment gates; an absent run is **NOT VERIFIED**, never implicit success.

## Next decision

On green CI: inspect mismatch ratio, dispersion, timeout negatives and race
tests. Candidate stays exploratory until game workload attribution confirms
that these calls are a meaningful CPU fraction. On CI failure: fix only the
feature branch, keep original baseline immutable.
