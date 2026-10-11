# KYTY-INDEX-001 / G2-B1 — bounded lifecycle fault-injection oracle

Issue: [Mathlab #289](https://github.com/definitely-stable/Mathlab/issues/289), see the preregistered G2-B1 protocol.
Scope: `MrFr3di/KytyPS5:perf/texture-range-index` ONLY. No edits to fork `main`/upstream and no local PC usage.

## Motivation and exact model boundary

G2-B0 passed 8,363 C++20 finite-model checks but tracks a global completed GPU tick rather than an individual image's last submitted use, and lacks systematic callback-fault mutation checks. G2-B1 adds a **separate** CPU-only state machine under `tests/TextureGcLifecycleFaultTests.cpp`, never linked into `kyty_emulator`.

Each simulated image/stencil member has an immutable-generation ID, `Free / Registered / Pending` lifecycle phase, parent reference, tracked views and synthetic bytes, hazards (GPU, CPU, buffer), and a last-use submission fence. The callback FIFO records a captured fence and sequence. Destruction is legal only if the matching callback is at the front and GPU completion covers its fence. The slot generation advances on physical erase, skipping zero on wrap. The test models monotone, **synthetic** submission/completion ticks, not a Vulkan driver. Hazard resolution is an explicit test event, **not a real GPU writeback**. All three unresolved hazard classes conservatively block offline root/stencil retirement; this is stricter than and does not replace production's ownership logic.

## Independent audit and required kill checks

`WellFormed` independently reconstructs callback references, sequence monotonicity, unique pending IDs, association phases, generation and root linkage, safe fence capture, guest-byte accounting, and live-view accounting. `Transition` independently audits changes from one step to the next, including the event that is allowed to alter each phase and exact generation change. Both return boolean verdicts, so intentionally invalid states can be tested without terminating the executable. This does **not** constitute a formal proof or a real GPU scheduler integration test.

18 named negative controls must be **rejected**: duplicate callback, stale callback generation, fence forged before last GPU use, queue reordering, missing callback despite balanced aggregates, GPU/CPU/buffer dirty pending state, orphaned stencil association, early physical erase, registered-byte/view-accounting corruption, fabricated completion, stale parent generation, use beyond submitted fence, dirty-state laundering during retirement, callback before GPU completion, and missing generation increment (ABA). Valid controls must demonstrate denied use/dirty/resolve/second retirement after logical removal, full normal drain at quiescence, and `UINT32_MAX → 1` generation rollover.

**Bounded CI coverage:** 16 initial hazard masks × 10,000 four-event action sequences = **160,000 exhaustive traces / 640,000 transitions**, plus 48 seeded replay lanes × 192 events = **9,216 additional transitions**. Seed `0xB121D0F1` is pinned; event alphabet covers use, mark, resolve, retire, complete, drain and create. No game binaries, files, user PC, Vulkan GPU, ETW, or network access are involved. These are deterministic finite traces, not probabilistic proof of the absence of bugs.

## Execution and evidence — only GitHub-hosted Windows

Both jobs in `.github/workflows/texture-range-index.yml` compile and execute the test on `windows-2022` x64 with `clang-cl`, C++20 Release. The fast job runs the executable directly; the full job uses CMake/CTest `texture_gc_lifecycle_fault` and separately exports its result, retaining full `kyty_emulator` build and CLI smoke. **All tests run in GitHub Actions. No local PC commands or hardware requirements.** CI archives:

- `texture-gc-lifecycle-faults-fast.csv` in the fast artifact;
- `texture-gc-lifecycle-faults.csv` in the full Windows artifact.

CSV format: `kind,name,value`, followed by `fault,<fault_name>,rejected`, coverage totals and `summary,TextureGcLifecycleFaultTests_PASS,1`. Nonzero exit or missing terminal summary fails CI. Output `decision,production_gc,NO_GO` is mandatory.

**G2-B1 CI MODEL ACCEPT** requires both hosted Windows jobs, all intentional mutants rejected, exact coverage outputs, existing G2-B0 oracle, CMake/CTest and unchanged full emulator/CLI checks. **Production dual-pressure GC is still NO-GO** without renderer-level GPU coherency/writeback, host synchronization and real bottleneck evidence; test results never assert FPS or Vulkan resource gains.

Khronos normative requirements: [vkDestroyImage](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyImage.html) VUID 01000; [vkDestroyImageView](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyImageView.html) VUID 01026. This test only approximates their command-completion requirement.
