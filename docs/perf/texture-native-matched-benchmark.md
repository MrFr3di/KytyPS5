# KYTY-INDEX-001 S4-B4b — native SlotId matched benchmark

**Research only**, exclusively fork branch perf/texture-range-index.
The source reference is TextureCache::FindImagesInRegion: its canonical
MultiLevelPageTable<InlinePageOwnerList<ImageId,16>,20,44,14>, genuine
Common::SlotVector/Common::SlotId generation identity, query_epoch dedup,
live overlap callback, and first-page registration ordering.

Two genuinely matched configurations: canonical_only uses native SlotVector
and canonical page table. canonical_plus_native_lazy includes ALL the same
objects and registration cost PLUS real NativeLazyRangeSidecar::Register,
TryFind and Unregister; every refused lookup executes the canonical fallback.
No prototype uint32 test identifiers, no duplicate sidecar-only baseline.

Three fixed SYNTHETIC workloads: 64 dense overlapping image ranges,
128 sparse disjoint registered points, and 96 churn-heavy ranges.
Each trace runs 320 small/large mixed queries twice with four warmups
before each pass. Between passes native SlotIds are destroyed and reinserted
with generation reuse; live effective ranges are mutated without changing
the original page ownership. Config: snapshot eligible at >=4 pages,
build after 4 eligible queries, 16-record block, 1MiB payload cap.

Before ANY timer starts, every lookup (cold, hot, churn and rebuilt) is
compared in exact ordered SlotId sequence to an independent page-major
oracle that scans live registration state. The benchmark also rejects
stale-generation reuse, omitted small-query canonical fallback and
missing snapshot rebuilding.

Each of two variants runs in a fresh process for each of 3 repetitions.
Variant execution order rotates between repetitions. Six timed stages:
initial registration; initial lazy snapshot warmup; steady-state lookups;
unregister/re-register churn; post-churn rebuild; post-churn lookups.
Report C++ new/delete allocation requests per phase, requested total
allocation bytes, tagged peak LIVE bytes and checkpoints (NOT RSS),
canonical allocated bucket count, H1 nominal payload (NOT full H1 memory),
snapshot builds, answered/fallback counts, and exact native checksum.

The Python runner rejects native checksum/ID/coverage mismatches and emits
raw per-process CSV plus median phase time and memory/allocations over
three repetitions. No fixed timing threshold: GitHub-hosted runners have
variable CPU load. Timing ratios refer to CPU-only synthetic conditions,
not GPU frames, game cold-hit distribution, Vulkan driver cost or FPS.
Production TextureCache and sidecar remain completely unchanged.
Integration stays NO-GO without a demonstrated end-to-end practical win
including registration/churn and real workload attribution.
