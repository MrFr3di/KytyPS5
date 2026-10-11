#ifndef KYTY_DESCRIPTOR_HEAP_TYPE_BUDGET_H_
#define KYTY_DESCRIPTOR_HEAP_TYPE_BUDGET_H_
// KYTY-INDEX-001 G3-D3: source-layout type cost, no Vulkan dependency.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
namespace Libs::Graphics {
enum class DescriptorBudgetKind : uint8_t {
  StorageBuffer=0, SampledImage=1, StorageImage=2, Sampler=3, Unsupported=4
};
class DescriptorLayoutBudget final {
public:
  void Add(DescriptorBudgetKind kind, uint32_t count) noexcept {
    const auto i=static_cast<size_t>(kind);
    if(i>=m_counts.size()) { m_known=false; return; }
    if(count>std::numeric_limits<uint64_t>::max()-m_counts[i]) {
      m_known=false; return;
    }
    m_counts[i]+=count;
  }
  [[nodiscard]] bool Known() const noexcept {return m_known;}
  [[nodiscard]] const std::array<uint64_t,4>& Counts() const noexcept{return m_counts;}
private:
  std::array<uint64_t,4> m_counts{};
  bool m_known=true;
};
} // namespace Libs::Graphics
#endif
