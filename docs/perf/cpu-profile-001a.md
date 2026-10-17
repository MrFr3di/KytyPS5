# CPU-PROFILE-001A — On-demand native synchronization observability

**Research branch:** `perf/cpu-runtime-research` only. Base merge:
`019f37714cec984e41ce582c6605518dacb395fd`, bringing in fork main
`6aa3846fb2b810fc0b7d75d8296e19d391c1a705`. No changes to fork
`main`, original upstream or the texture-range branch.

## Why this step

CPU-SYNC-001 (SyncOnAddress) and CPU-SYNC-002A/B/C (EventFlag and Semaphore
waiter drains) have Windows-hosted correctness evidence, but the synthetic
address-wait microbenchmarks exhibit severe drift and cannot establish FPS
benefits or the frequency of these operations in a running game.

The new fork main already provides `Common::DebugCounters`,
`HostStatsSampler` and an on-demand performance overlay. Reuse these rather
than introducing a second statistics system or a polling thread.

## Data and semantics

All counters use existing lock-free relaxed atomics and are updated only
when the performance panel is enabled. The following numbers appear in the
**CPU** section of the upstream performance overlay:

| Display | Recorded counters | Semantics |
|---|---|---|
| Address wait calls | `CpuAddressWaitCalls`, `CpuAddressImmediateMismatch` | Valid SyncOnAddress Wait32/64 calls/s and immediate value mismatch returns/s |
| Address registered waits | `CpuAddressRegistered`, `CpuAddressRegisteredNs` | Windows/macOS portable waiter registrations/s and cumulative elapsed time; Linux measures equivalent non-mismatch futex-loop entries |
| Native semaphore waits | `CpuSemaphoreWaitCalls`, `CpuSemaphoreBlocked`, `CpuSemaphoreBlockedNs` | Valid native kernel semaphore waits/s; blocked/registered calls/s and their elapsed wall duration |
| Event flag parks | `CpuEventFlagParks`, `CpuEventFlagParkNs` | CondVar wait attempts/s and elapsed wall time, not one counter per unique guest API call |
| Native object drains | `CpuSemaphoreDrainCalls`, `CpuSemaphoreDrainNs`, `CpuEventFlagDrainCalls`, `CpuEventFlagDrainNs` | Native semaphore destructor and EventFlag cancellation/deletion with outstanding waiters, and wall time until all registered waiters exit |

A counter is cumulative while recording is enabled; overlay rates are
differences between its snapshots. A recorded 3000 ms/s **does not imply 3 CPU
cores used**: multiple guest threads may block simultaneously. "Blocked",
"registered" and "drain" durations are elapsed wall-clock time, not sampled
CPU execution time. These totals include scheduler latency and callbacks.

These counters are deliberately *not* an emulation frame-time estimator or a
measurement of call-site attribution. They cannot separate context-switch
overhead from the time a guest thread legitimately waits for another thread.

## Cost and lifecycle

- The default enabled flag in upstream `DebugCounters` is false.
- Disabled fast paths pay an additional relaxed enabled-flag read when the
  added `Add()` site is reached. They perform **no time-point read and no
  counter increment** while disabled. A small branch overhead remains; this
  must be considered when interpreting tiny address-mismatch microbenchmarks.
- Enabling the panel instruments registered waits with
  `std::chrono::steady_clock` via the existing `ScopedTimer` and increments
  per-category atomic counts. This intentionally changes observed timing.
- Guest signals, ordering rules, cancellation, raw pointer lifetime and wake
  mechanisms are unmodified by CPU-PROFILE-001A.
- Capture counter rates with the panel open during the *same reproducible
  scene and workload* in at least two runs. Prefer recording 60 s of sustained
  activity after warmup, frame-time distribution and CPU thread utilization
  separately. Do not compare panel-on latency directly with panel-off
  synthetic timing.

The new main overlay is a Vulkan/game UI feature. GitHub-hosted headless CI
cannot supply representative guest PS5 workloads, FPS or shader/presentation
frame-time distributions. The counter plumbing is therefore validated in
standalone real-production-source tests rather than fictional gameplay runs.

## Acceptance gates

`SyncOnAddressTests.cpp` checks exactly one fast mismatch and one matching
zero-timeout registration under enabled profiling, and verifies disabled
profiling does not count calls. The immutable baseline test is compiled
without this candidate-only assertion. `EventFlagLifecycleTests.cpp` and
`SemaphoreLifecycleTests.cpp` check that actual registered waits increment
their counters and that disabling collection stops increments.

The Windows workflow must pass all existing correctness CTests, 10 repeated
semaphore stress suites, full headless emulator compilation (including the
new overlay) and CLI smoke. CTest success establishes instrumentation
wiring and unchanged tested semantics, **not** lower CPU use or game FPS.

After the first real game workload profile, prioritize optimizations by
observed call rate, blocked duration, guest CPU utilization and repeatable
frame-time impact. High wait time alone is not a reason to optimize a
correct blocking primitive.

**Current research interpretation:** `INSTRUMENTED`, performance and FPS
`NO_VERDICT` until representative workload measurements exist.
