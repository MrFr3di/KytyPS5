# KYTY-INDEX-001 S4-B5-A — canonical TextureCache bucket walk

Scope: fork MrFr3di/KytyPS5 feature branch perf/texture-range-index only;
Mathlab #289 documents the protocol. Never modify upstream or fork main.

## Source-backed optimization

The existing production TextureCache::FindImagesInRegion enumerates every
one-MiB guest page in the requested region and performs
m_image_page_table.Find(page). The canonical
MultiLevelPageTable<InlinePageOwnerList<ImageId,16>,20,44,14> already stores
1024 one-MiB pages per sparse L1 bucket (=1 GiB per bucket). The existing
ForEachAllocatedPageInRange in the SAME canonical index can skip never-allocated
L1 buckets while preserving ascending page order; allocated-but-now-empty
buckets are intentionally still traversed. No second index or metadata.

New portable source-executing ForEachCanonicalOwnerPage<false> performs the
original page.Find walk; <true> uses ForEachAllocatedPageInRange. Production
TextureCache uses the exact same owner callback with preserved query_epoch,
live Image::Overlaps, original registration/unregistration, result order,
invalid query behavior, and telemetry page count plus inspected owners.

CMake KYTY_TEXTURE_CANONICAL_BUCKET_SKIP defaults OFF. Only when ON does
kyty_emulator compile the optimized canonical path. Full Windows hosted CI
builds ON, but is still only a headless compile/CLI smoke, not a GPU test.

## Frozen correctness and timing

Four SYNTHETIC scenarios: dense overlapping pages; sparse multi-GiB queries
across never-allocated L1 buckets; heavy churn with SlotId generation reuse;
and guest upper address-space/L1 1023-1024 boundary. For each case, 96
queries before and 96 after 12 unregister/re-register mutations with
live effective-range changes. Both page walks must match independent
page-major/native SlotId oracle element-by-element including order,
deduplication and rounded overlap. Previously allocated/now-empty bucket
and invalid ranges are negative/edge cases.

24 isolated subprocesses across four scenarios, two variants and three
rotated repetitions. Per-stage registration, query, churn and post-query
nanoseconds, C++ operator-new REQUEST counts, canonical allocated bucket
count, owner visits and native result checksum recorded. New allocations
and bucket footprints must match across variants: no duplicate structure.
Timing threshold is NOT a correctness gate because GitHub-hosted shared
runner CPU performance is noisy. Python reports medians and raw results.

These synthetic CPU measurements do not demonstrate in-game workload
distribution, GPU latency, frame time, VRAM use, or FPS. The CPU fixture
uses an equivalent live effective-range predicate rather than compiling
Vulkan-dependent Image::Overlaps; source tripwires require production's
Image::Overlaps and query_epoch to remain unchanged. Production rollout
cannot be recommended without representative cold lookup and frame data.
