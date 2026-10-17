# CPU-SYNC-002C — Native semaphore lifecycle race oracle

## Context and scope

KytyPS5 fork, feature branch `perf/cpu-runtime-research`. The branch includes
fork main `c2173801d47f65d840487e9998bddcbb601c6691` through the two-parent
merge `5d0b35ee630783c4de8456ad22f041c19ad9c484`. The immutable baseline
`b82512be065b2fa2432cfa4e5a8c779604236299` is intentionally retained
*only* for the CPU-SYNC-001 microbenchmark, not used as the merged branch base.

CPU-SYNC-002B replaced only the native kernel-semaphore destructor's
10-microsecond sleep/relock loop with a dedicated `m_drained_cond_var`. The
last registered waiter calls `SignalAll()` after removal under `m_mutex`.
The destructor checks for an empty waiter vector while holding the same mutex
and uses `WaitFor(1000000)` to release it atomically. This also avoids
`CondVar::Wait()` guest-callback dispatch during host-only draining. A
long-duration timeout is a fallback; wake notification is the normal path.

### Race histories that must remain valid

1. **Signal versus Cancel:** with two registered waiters, one unit is signaled
   at the same time as cancel. Both operations linearize under `m_mutex`.
   If signal wins, exactly one waiter returns `OK` and one `ECANCELED`;
   the returned cancel count is 1. If cancel wins, both return `ECANCELED`,
   the cancel count is 2, and the late signal may remain as an unused token.
   No other result combination is legal.
2. **Partial signal then immediate delete:** with four registered waiters,
   signaling two units marks exactly two waiters `ready=Ok`; immediate
   destructor marks the other two `ready=Deleted`. All four release stack
   waiter storage before object destruction completes. Expected terminal
   results: two `OK`, two `EACCES`.
3. **Timeout versus Cancel:** a 100 ms timeout can expire before cancellation
   or be preempted by it. The cancellation count and one waiter's terminal
   result must agree: `(0, ETIMEDOUT)` or `(1, ECANCELED)`. The finite
   timeout budget cannot increase, and no waiter remains in the vector.
   Tests try immediate, 30 ms, and 140 ms post-registration cancellation.
4. **Weighted priority:** a priority-sorted waiter needing three units cannot
   prevent a lower-priority waiter needing one unit from progressing on two
   available units, matching the current `WakeWaiters()` policy. A later
   signal provides the final two units. Counters must total exactly.
5. **Cancel reset/reuse:** cancel with a new count must cancel existing
   waiters without letting them consume the reset tokens. Subsequent
   `KernelPollSema` consumes the tokens exactly once.

The older tests remain: 20×4-waiter Cancel→Delete, 20×4-waiter Delete,
FIFO vs priority ordering, zero timeout, maximum count, and guest APC polling.

### Test design

`tests/SemaphoreLifecycleTests.cpp` compiles the production
`src/kernel/semaphore.cpp` translation unit directly with a feature-only
`KYTY_SEMAPHORE_TEST_HOOKS` observer guarded by the same semaphore mutex.
The shims for guest thread priority, pending guest signals, host clock and
POSIX errno exist only in the isolated test target. No test hook or shim is
compiled into `kyty_emulator`.

The adversarial operations are coordinated only after all waiters have
registered. For concurrent Signal/Cancel, a shared atomic start flag releases
both operation threads; exact interleaving is deliberately unspecified, but
the set of permitted terminal outcomes is fully specified.

**Memory safety boundary:** `KernelDeleteSema` is never raced against an
untracked external method invocation on the same raw pointer. Callers join
Signal/Cancel producers before deletion. In lifecycle delete races only
waiters already registered and owned by the semaphore destructor can remain
active, as in the existing API's lifetime contract. Concurrent arbitrary
new API calls after deletion remain outside scope and may require a separate
handle-generation/registry design.

### CI

The Windows 2022 clang-cl workflow first compiles the complete isolated
correctness set and executes the five CTests. It then runs a dedicated
`ctest --repeat until-fail:10 -R ^semaphore_lifecycle$` gate and archives
`cpu-semaphore-stress.log`. This improves scheduler/interleaving coverage
but cannot exhaust all schedules or substitute for race detectors.

Only after those gates pass does it run the unchanged A-B-B-A
SyncOnAddress microbenchmark, build all of `kyty_emulator` and execute the
headless CLI smoke test.

**Acceptance:** each final HEAD requires a green hosted Windows workflow,
all repeated stress passes, no empty CTest selections, a full emulator build
and smoke. There is no measured semaphore-deletion latency or real gameplay
frame-time evidence. Thus green CI supports `CORRECTNESS_ACCEPT` for
CPU-SYNC-002C, **not** a performance/FPS claim. Existing semaphore ordering
and cancellation API policy remains unchanged.
