#include "graphics/host_gpu/renderer/cache/textureRangeTelemetry.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
using Libs::Graphics::TextureRangeTelemetry;
void Check(bool value, const char* message) {
  if (!value) { std::fprintf(stderr, "TextureRangeTelemetryTests: %s\n", message); std::abort(); }
}
void TestBinsAndGenerations() {
  TextureRangeTelemetry m;
  m.OnRegister(3);
  m.OnQuery(1, false, 3, 1, 10);
  m.OnQuery(2, true, 5, 2, 12);
  m.OnQuery(4, false, 4, 2, 14);
  m.OnQuery(8, true, 9, 1, 16);
  m.OnQuery(16, false, 11, 5, 18);
  m.OnQuery(64, false, 12, 1, 20);
  m.OnQuery(256, true, 13, 3, 22);
  m.OnInvalidQuery();
  Check(m.Data().queries == 7 && m.Data().invalid_queries == 1,
        "query and invalid range counts");
  Check(m.Data().page_overlap_queries == 3, "4KiB page mode count");
  for (auto value : m.Data().query_page_bins) Check(value == 1, "all seven page bins");
  Check(m.Data().owners_inspected == 57 && m.Data().results == 15,
        "owner visit and emitted counts");
  Check(m.Data().query_ns == 112, "elapsed lookup duration total");
  Check(m.Data().current_generation_large_queries == 4, "large range count");
  m.OnUnregister(3);
  Check(m.Data().completed_generations == 2 &&
            m.Data().generations_with_queries == 1 &&
            m.Data().max_large_queries_per_generation == 4 &&
            m.Data().active_images == 0, "generation closing");
  for (int i = 0; i < 10; ++i) m.OnQuery(8, false, 0, 0, 1);
  m.OnRegister(2);
  Check(m.Data().generations_with_eight_large_queries == 1 &&
            m.Data().max_large_queries_per_generation == 10,
        "completed qualifying generation");
  Check(m.Data().completed_generation_large_queries == 14 &&
            m.Data().completed_generation_large_queries_after_eight == 2 &&
            m.Data().generation_large_query_bins[0] == 1 &&
            m.Data().generation_large_query_bins[3] == 1 &&
            m.Data().generation_large_query_bins[4] == 1,
        "completed qualifying generation");
  Check(m.Data().max_active_images == 1 &&
            m.Data().registered_pages == 5 &&
            m.Data().unregistered_pages == 3, "registration page accounting");
  Check(m.Data().current_generation_large_queries == 0,
        "new registration resets large query count");
}
void TestDumpNoAddresses() {
  TextureRangeTelemetry m;
  m.OnRegister(123);
  m.OnQuery(2048, true, 42, 5, 150);
  std::FILE* file = std::tmpfile();
  Check(file != nullptr, "create anonymous CSV file");
  m.WriteCsv(file);
  std::rewind(file);
  std::string csv;
  char buffer[256]{};
  while (std::fgets(buffer, sizeof(buffer), file)) csv += buffer;
  std::fclose(file);
  Check(csv.find("metric,value\n") == 0, "stable metric CSV header");
  Check(csv.find("query_pages_256_plus,1") != std::string::npos,
        "long range bucket exported");
  Check(csv.find("current_generation_large_queries,1") != std::string::npos,
        "unfinished generation identified explicitly");
  Check(csv.find("queried_pages,2048") != std::string::npos,
        "sum counts but not guest addresses");
}
// Exact 1-MiB page geometry: L1 bucket contains 1024 pages, page count 2^24.
// Separate synthetic fixture; it is NOT game capture or real FPS evidence.
TextureRangeTelemetry MakeShapeFixture() {
  TextureRangeTelemetry t;
  struct Span { uint64_t first, last; };
  constexpr Span ranges[] = {
    {1023, 1024},  // 1 page; last page of L1 without crossing
    {1023, 1025},  // 2 pages; crosses L1
    {1000, 1064},  // 64 pages; crosses L1
    {100, 356},    // 256 pages; stays inside L1
    {900, 1156},   // 256 pages; crosses L1
    {0, 1024},     // 1024 pages; stays inside L1
    {0, 1025},     // 1025 pages; crosses L1
  };
  for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
    const auto [first, last] = ranges[i];
    const auto pages = last - first;
    t.OnQuery(pages, false, 0, 0, 10);
    Check(t.ObserveCanonicalSpan(first, last, 1024, uint64_t{1} << 24),
          "valid canonical span");
  }
  return t;
}
void TestCanonicalSpanShape() {
  TextureRangeTelemetry telemetry = MakeShapeFixture();
  const auto original = telemetry.Data();
  Check(original.queries == 7 && original.queried_pages == 1+2+64+256+256+1024+1025,
        "old metric semantics preserved");
  Check(original.shape_queries == 7 && original.shape_l1_cross_queries == 4,
        "shape total and L1-cross counts");
  Check(original.shape_pages_64plus == 5 &&
        original.shape_pages_256plus == 4 &&
        original.shape_pages_1024plus == 2, "page threshold boundaries");
  Check(original.shape_pages_256plus_same_l1 == 2 &&
        original.shape_pages_256plus_cross_l1 == 2,
        "cross/within L1 partition for T256");
  Check(!telemetry.ObserveCanonicalSpan(0, 0, 1024, uint64_t{1}<<24),
        "reject empty");
  Check(!telemetry.ObserveCanonicalSpan(300, 100, 1024, uint64_t{1}<<24),
        "reject inverted");
  Check(!telemetry.ObserveCanonicalSpan(100, 356, 0, uint64_t{1}<<24),
        "reject division by zero");
  Check(!telemetry.ObserveCanonicalSpan((uint64_t{1}<<24)-1,
        (uint64_t{1}<<24)+1, 1024, uint64_t{1}<<24),
        "reject beyond 44-bit address space");
  Check(!telemetry.ObserveCanonicalSpan(0, UINT64_MAX, 1024,
                                       uint64_t{1}<<24),
        "reject out-of-range last page");
  const auto& after = telemetry.Data();
  Check(after.shape_queries == original.shape_queries &&
        after.shape_l1_cross_queries == original.shape_l1_cross_queries &&
        after.shape_pages_256plus == original.shape_pages_256plus,
        "invalid observations cannot affect counters");
}
}
int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--shape-fixture") {
    // Sole purpose: strictly labeled synthetic fixture for offline CI parser.
    MakeShapeFixture().WriteCsv(stdout);
    return 0;
  }
  TestBinsAndGenerations();
  TestDumpNoAddresses();
  TestCanonicalSpanShape();
  std::puts("TextureRangeTelemetryTests: counters and CSV PASS");
  return 0;
}
