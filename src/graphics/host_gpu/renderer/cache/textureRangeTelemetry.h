#ifndef KYTY_GRAPHICS_TEXTURE_RANGE_TELEMETRY_H_
#define KYTY_GRAPHICS_TEXTURE_RANGE_TELEMETRY_H_

// KYTY-INDEX-001/S4-B. Bounded, address-free aggregate diagnostics.
// No IDs, virtual addresses, descriptors or per-query trace buffers stored.
// Callers serialize all operations under the TextureCache lock. Build-time
// opt-in only; the released emulator has no telemetry instrumentation.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Libs::Graphics {

class TextureRangeTelemetry final {
public:
  static constexpr size_t kPageBins = 7;
  struct Counters {
    uint64_t queries = 0;
    uint64_t invalid_queries = 0;
    uint64_t page_overlap_queries = 0;
    uint64_t owners_inspected = 0;
    uint64_t results = 0;
    uint64_t queried_pages = 0;
    uint64_t query_ns = 0;
    uint64_t registrations = 0;
    uint64_t unregistrations = 0;
    uint64_t registered_pages = 0;
    uint64_t unregistered_pages = 0;
    uint64_t active_images = 0;
    uint64_t max_active_images = 0;
    uint64_t completed_generations = 0;
    uint64_t generations_with_eight_large_queries = 0;
    uint64_t max_large_queries_per_generation = 0;
    uint64_t current_generation_large_queries = 0;
    uint64_t current_generation_queries = 0;
    uint64_t generations_with_queries = 0;
    std::array<uint64_t, kPageBins> query_page_bins{};
    std::array<uint64_t, kPageBins> query_time_ns_by_page_bin{};
  };

  void OnRegister(uint64_t pages) noexcept {
    FinishGeneration();
    ++m_data.registrations;
    m_data.registered_pages += pages;
    ++m_data.active_images;
    m_data.max_active_images = std::max(m_data.max_active_images, m_data.active_images);
  }
  void OnUnregister(uint64_t pages) noexcept {
    FinishGeneration();
    ++m_data.unregistrations;
    m_data.unregistered_pages += pages;
    if (m_data.active_images != 0) --m_data.active_images;
  }
  void OnInvalidQuery() noexcept { ++m_data.invalid_queries; }
  void OnQuery(uint64_t pages, bool page_overlap, uint64_t inspected,
               uint64_t results, uint64_t elapsed_ns) noexcept {
    if (pages == 0) {
      OnInvalidQuery();
      return;
    }
    ++m_data.queries;
    ++m_data.current_generation_queries;
    m_data.page_overlap_queries += static_cast<uint64_t>(page_overlap);
    m_data.owners_inspected += inspected;
    m_data.results += results;
    m_data.queried_pages += pages;
    m_data.query_ns += elapsed_ns;
    const auto bin = PageBin(pages);
    ++m_data.query_page_bins[bin];
    m_data.query_time_ns_by_page_bin[bin] += elapsed_ns;
    if (pages >= 8) ++m_data.current_generation_large_queries;
  }

  [[nodiscard]] const Counters& Data() const noexcept { return m_data; }

  // Fixed CSV shape suitable for automated comparison. The current generation
  // is reported separately rather than pretending it has already completed.
  void WriteCsv(std::FILE* output) const noexcept {
    if (!output) return;
    std::fprintf(output, "metric,value\n");
    const auto emit = [output](const char* name, uint64_t value) {
      std::fprintf(output, "%s,%llu\n", name, static_cast<unsigned long long>(value));
    };
    emit("queries", m_data.queries);
    emit("invalid_queries", m_data.invalid_queries);
    emit("page_overlap_queries", m_data.page_overlap_queries);
    emit("owners_inspected", m_data.owners_inspected);
    emit("results", m_data.results);
    emit("queried_pages", m_data.queried_pages);
    emit("query_ns", m_data.query_ns);
    emit("registrations", m_data.registrations);
    emit("unregistrations", m_data.unregistrations);
    emit("registered_pages", m_data.registered_pages);
    emit("unregistered_pages", m_data.unregistered_pages);
    emit("active_images_at_dump", m_data.active_images);
    emit("max_active_images", m_data.max_active_images);
    emit("completed_generations", m_data.completed_generations);
    emit("generations_with_queries", m_data.generations_with_queries);
    emit("generations_with_eight_large_queries", m_data.generations_with_eight_large_queries);
    emit("max_large_queries_per_completed_generation", m_data.max_large_queries_per_generation);
    emit("current_generation_queries", m_data.current_generation_queries);
    emit("current_generation_large_queries", m_data.current_generation_large_queries);
    static constexpr const char* kNames[kPageBins] = {
        "1", "2_3", "4_7", "8_15", "16_63", "64_255", "256_plus"};
    for (size_t i = 0; i < kPageBins; ++i) {
      std::fprintf(output, "query_pages_%s,%llu\n", kNames[i],
                   static_cast<unsigned long long>(m_data.query_page_bins[i]));
      std::fprintf(output, "query_ns_pages_%s,%llu\n", kNames[i],
                   static_cast<unsigned long long>(m_data.query_time_ns_by_page_bin[i]));
    }
  }
private:
  static size_t PageBin(uint64_t pages) noexcept {
    if (pages == 1) return 0;
    if (pages <= 3) return 1;
    if (pages <= 7) return 2;
    if (pages <= 15) return 3;
    if (pages <= 63) return 4;
    if (pages <= 255) return 5;
    return 6;
  }
  void FinishGeneration() noexcept {
    ++m_data.completed_generations;
    m_data.generations_with_queries += (m_data.current_generation_queries != 0);
    m_data.generations_with_eight_large_queries +=
        (m_data.current_generation_large_queries >= 8);
    m_data.max_large_queries_per_generation =
        std::max(m_data.max_large_queries_per_generation,
                 m_data.current_generation_large_queries);
    m_data.current_generation_queries = 0;
    m_data.current_generation_large_queries = 0;
  }
  Counters m_data{};
};

} // namespace Libs::Graphics
#endif // KYTY_GRAPHICS_TEXTURE_RANGE_TELEMETRY_H_
