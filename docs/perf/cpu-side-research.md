# CPU-side performance research — KYTY-CPU-001

Updated: 2026-10-17. Tracking: [Mathlab #582](https://github.com/definitely-stable/Mathlab/issues/582).

## Repository and immutable scope

- Fork: `MrFr3di/KytyPS5`; **all source, test, documentation and workflow writes only to `perf/texture-range-index`**.
- Fork `main` and original upstream are read-only. No PR or merge into `main`, no force pushes, no self-hosted runners.
- This branch absorbed fork main `b82512be065b2fa2432cfa4e5a8c779604236299` on 2026-10-17 via two-parent merge `465fd713b3268d843d1bf496dc510562b701cfe4`, preserving its texture research history.
- **New focus is host CPU time:** native guest x86-64 execution and patching, kernel HLE and synchronization, memory/address-space bookkeeping, thread scheduling and syscall dispatch. GPU/shader throughput is outside this research plan.
- Avoid mistaking texture-index CPU work for general guest-CPU execution improvements.

## CPU execution model: observe before replacing

On compatible x86-64 hosts Kyty executes guest code natively where possible; `loader/guestInstructionPatcher.cpp`, `loader/x64InstructionEmulator.cpp`, `loader/runtimeLinker.cpp` implement guest instruction/ABI compatibility. Therefore a generic interpreter-dispatch or whole-program JIT optimization proposal is unsupported without a profile indicating that path is actually hot.

Do not assume `std::map` or a lock is the bottleneck from code inspection alone. We must separately quantify cold load-time/patch work, stable frame-time CPU work, and infrequent exception/fallback work.

## Priority and required evidence

| ID | Subsystem | Candidate hypothesis | First evidence before further production changes |
|---|---|---|---|
| CPU-SYNC-001 | `kernel/syncOnAddress.cpp` | Windows portable mismatch waits unnecessarily construct and register a waiter, including allocating a shared entry, when the value already differs | Standalone 32/64-bit mismatched + matched zero-timeout CPU benchmark; existing SyncOnAddress race/callback/timeout tests |
| CPU-SYNC-002 | `kernel/eventFlag.cpp`, `kernel/semaphore.cpp` | Repeated 10-us sleep/poll in cancel/delete waiter draining causes scheduler overhead under contention | Wake/cancel/delete priority-order and concurrent timeout tests with a distribution of waiter counts; only then consider an event-driven design |
| CPU-MEM-001 | `kernel/memory.cpp`, `kernel/memoryAddressSpace.inc` | Ordered maps and recursive lock serialization may dominate memory-map-intensive guest calls | Sample real HLE call counts and per-path lock hold times; concurrent map/unmap/access correctness oracles |
| CPU-EXEC-001 | `loader/x64InstructionEmulator.cpp`, `loader/guestInstructionPatcher.cpp`, `loader/runtimeLinker.cpp` | Fallback instruction emulation, code patch discovery or ABI import dispatch may be expensive | Counters that separate load-time patching from hot instruction paths; sampled Windows call stacks where available |
| CPU-HLE-001 | `kernel/eventQueue.cpp`, `kernel/pthread.cpp` | Poll slice and event lifetime handling may create unnecessary wakeups or timer scans | Deterministic event replay + CPU time/latency distribution, preserving interrupt/signal semantics |

The source audit identifies candidates, **not measured bottlenecks**. No FPS claims without representative game traces and frame-time evidence.

## CPU-SYNC-001 current slice

The Windows/macOS portable `WaitPortable` registers a `PortableWaiter` inside `PortableAddressRegistry` before checking whether `ReadWord(address) != expected`. This incurs mutex, map and possible allocation overhead on a trivially satisfied wait. The change adds an acquire-load comparison **before** registration. The slow path still registers under the entry lock and checks the value again, preventing a lost wake. `WaitImpl` retains its single guest-signal callback on successful/mismatched exits. Linux futex code remains unchanged.

Test matrix: `sync_on_address_tests` (address validation, 32/64-bit mismatch, guest signal safepoint, matching zero timeout, wake and timeout races) and `event_queue_lifetime_tests` (neighboring CPU subsystem). The separate `sync_on_address_cpu_bench` prints `mismatch32`, `mismatch64`, `matching32_zero_timeout`, `matching64_zero_timeout` with 9 rounds of median/min/max ns/op. The zero-timeout cases are *not* comparable to mismatch latency; they exercise a different path.

The benchmark avoids a timing-based correctness assertion. It is **synthetic CPU evidence**, not an estimate of FPS gain. Hosted runner load, turbo-frequency changes, thread placement and cache state affect reported values; use paired runs on the same runner configuration and a large benefit over noise before accepting further optimizations.

### Acceptance gate

1. Windows x64 hosted CMake builds `kyty_emulator`, `sync_on_address_tests`, `event_queue_lifetime_tests`, `sync_on_address_cpu_bench`.
2. Both correctness suites PASS; bench prints `CPU_SYNC_001_PASS`, no callback count changes, address validation unchanged.
3. Results archived as `sync-on-address-cpu.csv` under the existing branch workflow's artifacts.
4. Mark performance impact as **UNVERIFIED** until new workflow run is complete; microbenchmark win is only a per-call improvement, never a gameplay proof.

## Modern practice and sources

- Microsoft [WaitOnAddress](https://learn.microsoft.com/en-us/windows/desktop/api/synchapi/nf-synchapi-waitonaddress): addresses permit uncontended fast paths, but waiters must recheck values after wake/spurious notification. Native Windows primitives are a *future* candidate only, after proving wake-count and guest-signal semantics.
- Google Benchmark [Reducing Variance](https://github.com/google/benchmark/blob/main/docs/reducing_variance.md): shared runners and CPU-frequency scaling can distort small differences. Repeat and report dispersion.
- AMD uProf 5.3 ([June 2026 Hotspots Guide](https://docs.amd.com/r/en-US/68658-uProf-getting-started-guide/AMD-uProf-Hotspots-Analysis)): sample CPU hot paths before touching broad architecture. Privileged hardware-counter modes may be unavailable on GitHub-hosted runners; portable wallclock or codegen evidence is supplemental, not equivalent to hardware sampling.

## Next slice

After confirming CPU-SYNC-001 CI correctness, profile or build a deterministic lifecycle oracle for `eventFlag` cancellation and deletion under 1/2/4/8 waiters, with explicit wake-order, timeout and signal-safety invariants. Do not change to `std::atomic::wait`, `WaitOnAddress`, spinlocks, or introduce a new dynamic recompiler based solely on synthetic one-shot timings.
