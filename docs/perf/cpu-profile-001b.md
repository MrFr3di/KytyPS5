# CPU-PROFILE-001B — Guest HLE origins and actual address parking

## Baseline and rationale

Feature branch: `perf/cpu-runtime-research` only; fork `main` unchanged.
Predecessor `CPU-PROFILE-001A` was accepted by GitHub-hosted Windows
[run #39035473204](https://github.com/MrFr3di/KytyPS5/actions/runs/39035473204),
including all isolated oracles, 10 semaphore stress repeats, a full headless
emulator build and CLI smoke.

The earlier overlay tracked `SyncOnAddress` calls, immediately mismatched
values, and *registered* waits. This is insufficient to infer the origin or
actual blocking rate: a value equal to the expected word with a zero timeout
may register in the portable waiter map and then return without a single
condition-variable wait.

## Instrumentation

Reuse only upstream `Common::DebugCounters` and the existing performance
overlay. No sampling thread, stack unwinding, guest address capture, new
production lock or third-party profiler is introduced.

### Where guest address operations originate

Five counter buckets correspond to named wrappers in
`src/libs/libKernel.cpp`:

| HLE source | Counter | Definition |
|---|---|---|
| legacy kernel address wait | `CpuGuestLegacyAddressWaits` | `KernelSyncOnAddressWait` |
| experimental typed address wait | `CpuGuestTypedAddressWaits` | `KernelSyncOnAddressWait32` and `KernelSyncOnAddressWait64` |
| POSIX-compatible umtx wait | `CpuGuestUmtxAddressWaits` | `UmtxOp` operation `UMTX_OP_WAIT` |
| kernel wake | `CpuGuestKernelAddressWakes` | `KernelSyncOnAddressWake` |
| POSIX-compatible umtx wake | `CpuGuestUmtxAddressWakes` | `UmtxOp` operation `UMTX_OP_WAKE` |

`RecordGuestSyncEntry()` maps each enumerated source to exactly one
lock-free `Add()` counter, returning without update when disabled.
`tests/CpuSyncEntryCountersTests.cpp` exercises the *same mapping function*
used by wrappers, including 4×1000 simultaneous increments for **each**
source, disabled-mode no-ops, and an unrecognized value.

**Measurement unit:** HLE entrypoints/s, including invalid requests that
reach the corresponding wrapper. It is not a stack trace, guest PC, title,
instruction sample or proof that the call blocked. A direct internal
`SyncOnAddress::Wait32/64` call is counted in core wait totals but **not**
in any guest-HLE source bucket.

### What actually parks

`CpuAddressParkCalls` and `CpuAddressParkNs` count entries into
`PortableWaiter::condition.WaitFor()` or the Linux `FUTEX_WAIT_PRIVATE`
syscall and measure the elapsed wall-clock duration *around the blocking
primitive itself*. Compare against `CpuAddressRegistered`: the latter
includes portable slow-path registrations that never reached `CondVar::WaitFor`,
especially matching zero-timeout waits **on Windows**. On Linux, the
registration analogue is the non-mismatch futex loop, and a matching
zero-timeout wait **does attempt one futex syscall with an immediately
expiring timespec**. Thus `CpuAddressParkCalls` is a count of completed
blocking-primitive **invocations**, not proof of a scheduler context switch.
Do not compare this ratio across OSes as if the paths were identical.

The instrumented portable wait path remains:

1. Check the atomic word and immediately return when it mismatches.
2. Construct the existing registered-wait timer and add the waiter to the
   address registry under the original mutex.
3. Check the word and timeout under the same lock. **Only when actually
   calling `WaitFor`**, construct the separate park timer.
4. Perform the original signal-safe-point polling and unregister exactly as
   before.

No user-visible emulated return codes, wake policies, or lock ordering change.

### Overlay fields

In the upstream in-game performance overlay's CPU section:

- `Guest wait HLE calls`: legacy, typed and umtx operations per second.
- `Guest wake HLE calls`: kernel and umtx operations per second.
- `Actual address parks`: number of condition/futex waits and elapsed
  wall time per second.
- `Address registered waits`: the older matching-value/registry entry
  total, retained for comparison.

**Important:** elapsed wall time across blocked threads can exceed one
second per wall second. This is **not CPU time**, and larger waits do not
inherently mean CPU overhead. Neither the overlay nor GitHub-hosted runner
contains representative game workloads; no FPS/performance acceleration
is claimed from these metrics alone.

### Disabled overhead, consent and caveats

The upstream debug counters default to disabled and are enabled when the
performance overlay is visible. The new HLE wrapper classifier still
executes an inlined switch/branch before the disabled `Add()` returns.
The park timer checks the enabled flag without taking a timestamp while
disabled. The fast mismatch path does **not** start the park timer.

This implies small unavoidable instrumentation overhead even when
diagnostics is off. Future tiny-nanosecond microbenchmark comparisons must
account for it; the profile itself must be observed under a representative
scene (ideally repeatable FPS/frame-time and hot thread utilization), not
declared zero-cost.

Crossing the enable/disable boundary in the middle of an active timer can
discard that individual duration because upstream `ScopedTimer` requires
collection enabled at destruction. Accordingly, short acquisition windows
after toggling the overlay must not be interpreted as lossless event logs.

## Acceptance

- `CpuSyncEntryCountersTests.cpp`: exact 5-bucket mapping, disabled
  behavior, concurrent count preservation.
- Candidate-only `SyncOnAddressTests.cpp`: zero-timeout matching wait
  is registered but **does not park**; positive finite timed wait actually
  parks; invalid-address and original correctness cases still pass.
- All existing event flag, semaphore and address wait CTests; 10 stress
  repeats; full Windows x64 `kyty_emulator`; headless smoke.
- No new source changes to main, upstream or the old texture branch.

Acceptance establishes **instrumentation correctness at tested API
boundaries**, not the cause of gameplay stalls or an FPS gain. A next
candidate after representative game sampling is per-callsite
attribution for whichever source category actually dominates, ideally
without raw guest PC or per-event allocations in the hot path.
