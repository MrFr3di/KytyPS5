# KYTY-INDEX-001 / G0 — End-to-end texture workload attribution (Windows)

Tracking issue: [Mathlab #289](https://github.com/definitely-stable/Mathlab/issues/289).
Code branch: `MrFr3di/KytyPS5:perf/texture-range-index`; no merge into main.

## Purpose and decision boundary

Earlier range-index experiments isolated `FindImagesInRegion` but did not
establish its contribution to renderer CPU work or frame time. This G0 diagnostic
instruments the *existing* image/buffer/descriptor path before choosing G1/G2/G3
optimizations. **It does not enable the H0/H1 candidate or modify the page table.**

No guest addresses, shader identifiers, descriptor contents, filenames,
individual requests or game data are included in the CSV. The data is a set of
fixed-size per-stage aggregate counters, with no per-query buffers.

## How to enable

```powershell
cmake -S . -B _Build/workload-diagnostic -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DKYTY_BUILD_LAUNCHER=OFF `
  -DKYTY_TEXTURE_WORKLOAD_TELEMETRY=ON `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build _Build/workload-diagnostic --target kyty_emulator --parallel 3
$env:KYTY_TEXTURE_WORKLOAD_TRACE = '1'
# Run a representative PS5 workload using the usual emulator entrypoint.
# Quit normally; output is emitted by TextureCache destruction into current working directory.
Remove-Item Env:KYTY_TEXTURE_WORKLOAD_TRACE
python scripts/analyze_texture_workload_attribution.py --json .\texture-workload-attribution.csv
```

Both independent switches are mandatory. CMake `KYTY_TEXTURE_WORKLOAD_TELEMETRY`
defaults to **OFF**. Runtime variable must equal literal `1`. The old
`KYTY_TEXTURE_RANGE_TELEMETRY` / `KYTY_TEXTURE_RANGE_TRACE` switches are
separate. Windows GitHub Actions builds a telemetry-ON executable but deliberately
leaves the runtime variable unset, tests the absence of CSV output and does
**not** run PS5 games in CI.

File `texture-workload-attribution.csv` is overwritten each normal exit;
crashes or unwritable process directories can prevent output. Keep session
configuration separate from the anonymous counters (title, runtime duration,
scene, driver, GPU, build SHA, display settings, build mode).

## Schema

`kind,name,count,inclusive_ns,self_ns`

- `stage`: elapsed wall-clock nanoseconds for measured operation, including
  nested work, vs `self_ns` excluding nested **instrumented** scopes on the
  same thread. `count` is invocations, not page count. Scope nesting is tracked
  in thread-local state and totals use relaxed atomic adds.
- `event`: count (for rebind decision, number of descriptor writes, ranges,
  batches, requests, or bytes) with zero `inclusive_ns,self_ns`. Names ending
  in `_bytes` identify requested/staged/packed guest volumes, **not** actual
  PCIe/GPU transfers. The metric set is G1-A extended; archived older CSVs
  with missing metrics fail strict schema validation and must not be silently
  compared with the expanded schema.

Main diagnostic stages:
- `region_lookup` — canonical 1-MiB page-owner lookup.
- `image_discovery` and `resolve_overlap` — full image resolution, nested
  overlap work and associated actions.
- `image_refresh`, `image_initialize`, `image_copy`, `texture_readback`.
- `cpu_alias_invalidate`, `gpu_memory_invalidate`, `process_readbacks`.
- `register_image`, `unregister_image`, `garbage_collect`.
- `resolve_texture`, `rebind_images`, `descriptor_commit`.
- `buffer_acquire`, `buffer_synchronize`, `buffer_upload`,
  `buffer_download`, `buffer_garbage_collect`.

Events `rebind_reuse/rebind_missing/rebind_unregistered/rebind_needed` cover
the current `RenderExecutor::RebindImages` decision, and
`descriptor_push_writes/descriptor_set_writes` count Vulkan descriptor write
records on the two binding paths. This does not directly count expensive GPU
driver validation or descriptor-cache hits upstream of this call.

## G1-A: safe transfer-granularity attribution

- `buffer_upload_regions` counts nonempty buffer copy records selected by
  `MemoryTracker::ForEachUploadRange` and passed to `UploadCopies`;
  `buffer_upload_batches` counts the corresponding nonempty staging batches.
  Neither metric is a count of unique dirty pages or actual GPU submissions.
  `RegionManager::ForEachModifiedRange` currently returns 4-KiB tracker-page
  aligned spans: `buffer_upload_tracker_page_instances = staged_bytes / 4096`.
  This counts processed page *instances*, not unique pages across the trace.
- `buffer_upload_staged_bytes` measures requested payload staging bytes.
- `buffer_readback_regions` counts readback copy regions,
  `buffer_readback_payload_bytes` sums useful copy sizes, and
  `buffer_readback_packed_bytes` includes per-region 64-byte alignment.
  The packing ratio is a host staging layout metric, not PCIe amplification.
- `image_upload_requests` counts completed calls to `UploadImage` from
  `InitializeImage`; `image_upload_guest_bytes` sums the full guest backing
  footprints requested. These do not measure the native compressed/tiled GPU
  transfer size, and do not establish that partial image transfers are safe.
- Counters are incremented only with both telemetry opt-ins; disabled builds
  have no new runtime work. No guest addresses or transient trace buffers are
  stored.

**G1-A GO**: representative game captures demonstrate a material recurring
full-image/fragmented buffer-upload cost, supported by an external CPU/ETW
profile and correctness baseline. **G1-A NO-GO**: no representative evidence,
or measurement overhead masks the signal. This stage intentionally changes
only instrumentation and cannot establish gameplay speedup.

## Measurement interpretation and limitations

```powershell
python scripts/analyze_texture_workload_attribution.py texture-workload-attribution.csv
python -m unittest discover -s tests -p "test_texture_workload_attribution_analyzer.py" -v
```

The analyzer strictly validates field names, duplicates, unsigned values,
incomplete reports, `self_ns <= inclusive_ns`, and event time zeros. It reports
the largest measured self-time stages, rebind reason share and approximate
byte counters. Its verdict intentionally remains
`INSUFFICIENT_FOR_PRODUCTION_GO`.

**Important:** `steady_clock` is elapsed host wall time and includes waiting
and time-slice preemption; it is *not* retired CPU cycles. A sum of nested
measured `self_ns` is not an exhaustive process CPU or frame-time denominator.
Some work can happen on different threads. Values are not GPU timestamps.
The index self-time share of measured scopes is **not** the index share of the
full frame. Report only instrumented call-path attribution.

Because the diagnostic takes clock samples and atomic updates, it changes
timings. Compare identical binaries with identical flags and scenes, use repeated
captures, and preferably complement with an external sampling CPU profiler,
GPU timestamps and native ETW/Windows Performance Recorder traces. Keep
uninstrumented Release builds as a separate control.

## Follow-on GO/NO-GO tests

1. Verify whether region lookup is actually important relative to image
   resolve/refresh, buffer upload/readback, shader/descriptor work, GC, and waits.
2. Measure real guest-memory dirty-range sizes and mutation frequencies before
   attempting incremental refresh or another residency manager.
3. Instrument scheduler wait points and per-frame samples separately if the
   workload indicates a synchronization or stutter bottleneck. No GPU stall
   claim is supported by these timing counters alone.
4. Implement only one targeted G1/G2/G3 change with a correctness oracle.
   No native sidecar production deployment based solely on isolated CPU tests.

All experiment notes, CI run links and deployment decisions belong in
[Mathlab issue #289](https://github.com/definitely-stable/Mathlab/issues/289).
