// KYTY-INDEX-001 S3-C: isolated Windows CPU allocation and memory ledger.
// One variant/owner-count per process: every CSV row is comparable with the
// same executable and a fresh GitHub-hosted Windows process. This counts C++
// operator-new allocation *requests*, not malloc inside external runtimes.
// It reports sampled private commitment and process peak working set, NOT an
// exact per-index RSS or proof of actual emulator performance.
#include "graphics/host_gpu/renderer/cache/imageRangeHybrid.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#include <psapi.h>
#endif

namespace {
thread_local bool g_count_allocations = false;
thread_local uint64_t g_calls = 0;
thread_local uint64_t g_bytes = 0;
inline void Account(std::size_t bytes) {
  if (g_count_allocations) {
    ++g_calls;
    g_bytes += bytes;
  }
}
} // namespace

void* operator new(std::size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc{};
  Account(n);
  return p;
}
void* operator new[](std::size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc{};
  Account(n);
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#ifdef _WIN32
void* operator new(std::size_t n, std::align_val_t alignment) {
  void* p = _aligned_malloc(n ? n : 1, static_cast<std::size_t>(alignment));
  if (!p) throw std::bad_alloc{};
  Account(n);
  return p;
}
void* operator new[](std::size_t n, std::align_val_t alignment) {
  void* p = _aligned_malloc(n ? n : 1, static_cast<std::size_t>(alignment));
  if (!p) throw std::bad_alloc{};
  Account(n);
  return p;
}
void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
#endif

namespace {
using Libs::Graphics::HybridImageRangeIndex;
constexpr uint64_t MiB = 1ull << 20;
constexpr uint64_t base = 1ull << 32;

struct ProcessMemory {
  uint64_t private_usage = 0;
  uint64_t working_set = 0;
  uint64_t peak_working_set = 0;
};
ProcessMemory ReadMemory() {
  ProcessMemory m;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters))) {
    m.private_usage = static_cast<uint64_t>(counters.PrivateUsage);
    m.working_set = static_cast<uint64_t>(counters.WorkingSetSize);
    m.peak_working_set = static_cast<uint64_t>(counters.PeakWorkingSetSize);
  }
#endif
  return m;
}

struct AllocationRecord { uint64_t calls = 0, requested = 0; };
void AssertSequence(uint32_t count, const auto& ids) {
  if (ids.size() != count) std::abort();
  uint32_t expected = 1;
  for (uint32_t id : ids) if (id != expected++) std::abort();
}
AllocationRecord Measure(HybridImageRangeIndex& index, int mode,
                         uint32_t count, uint64_t address, uint64_t size) {
  g_calls = g_bytes = 0;
  g_count_allocations = true;
  for (int q = 0; q < 64; ++q) {
    if (mode == 0 || mode == 3) {
      auto result = index.Find(address, size);
      AssertSequence(count, result);
    } else {
      auto result = index.FindInline(address, size);
      AssertSequence(count, result);
    }
  }
  g_count_allocations = false;
  return {g_calls, g_bytes};
}
int Run(int mode, uint32_t owners) {
  if (mode < 0 || mode > 4 || (owners != 1 && owners != 16 && owners != 40)) return 2;
  HybridImageRangeIndex::Config cfg;
  cfg.snapshot_min_pages = 8;
  cfg.snapshot_after_large_queries = 1; // measure fully built steady state
  cfg.allow_snapshot = mode >= 3;
  cfg.skip_empty_buckets = mode >= 2;
  const auto before = ReadMemory();
  HybridImageRangeIndex index(cfg);
  for (uint32_t id = 1; id <= owners; ++id)
    if (!index.Register(id, base + static_cast<uint64_t>(id) * 8, 16 * MiB)) return 3;
  const auto registered = ReadMemory();
  if (mode >= 3) {
    // Trigger the lazy snapshot before allocation-rate measurement.
    const auto warm = index.Find(base + 8 * owners, 64 * MiB);
    AssertSequence(owners, warm);
    if (!index.HasValidSnapshot()) return 4;
  }
  const auto warm_memory = ReadMemory();
  const auto small_measure = Measure(index, mode, owners, base + owners * 8, 4096);
  const auto wide_measure = Measure(index, mode, owners, base + owners * 8, 64 * MiB);
  const auto after = ReadMemory();
  if (!index.Validate()) return 5;
  const char* labels[] = {"legacy_vector","legacy_inline","H0_inline",
                          "H1_vector","H1_inline"};
  std::printf("%s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%zu\n",
              labels[mode],owners,
              static_cast<unsigned long long>(small_measure.calls),
              static_cast<unsigned long long>(small_measure.requested),
              static_cast<unsigned long long>(wide_measure.calls),
              static_cast<unsigned long long>(wide_measure.requested),
              static_cast<unsigned long long>(before.private_usage),
              static_cast<unsigned long long>(registered.private_usage),
              static_cast<unsigned long long>(warm_memory.private_usage),
              static_cast<unsigned long long>(after.private_usage),
              static_cast<unsigned long long>(after.working_set),
              static_cast<unsigned long long>(after.peak_working_set),
              static_cast<unsigned long long>(index.Generation()),
              index.SnapshotBuilds());
  return 0;
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fputs("usage: image_range_alloc_bench <variant 0..4> <owners 1|16|40>\n", stderr);
    return 2;
  }
  return Run(std::atoi(argv[1]), static_cast<uint32_t>(std::atoi(argv[2])));
}
