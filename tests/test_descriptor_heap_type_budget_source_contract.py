"""G3-D3: source-backed Vulkan layout creation rather than draw writes."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
PATHS=[
 "src/graphics/host_gpu/renderer/pipeline/shaders.cpp",
 "src/graphics/host_gpu/renderer/pipeline/pipelineCache.h",
 "src/graphics/host_gpu/renderer/pipeline/descriptors.cpp",
 "src/graphics/host_gpu/renderer/pipeline/descriptorHeap.cpp",
 "src/graphics/host_gpu/renderer/pipeline/descriptorHeap.h",
]
def read():return [(ROOT/p).read_text(encoding="utf-8") for p in PATHS]
def audit(s,p,b,h,d):
    start=s.find("static void CreateDescriptorLayout(")
    end=s.find("// NOLINTNEXTLINE",start)
    if start<0 or end<=start:return False
    fn=s[start:end]
    required=(
      "std::span<const vk::DescriptorSetLayoutBinding> bindings",
      "create.pBindings    = bindings.data();",
      "graphics.device.createDescriptorSetLayout(",
      "DescriptorLayoutBudget type_budget;",
      "for (const auto& binding: bindings)",
      "switch (binding.descriptorType)",
      "type_budget.Add(DescriptorBudgetKind::StorageBuffer, binding.descriptorCount)",
      "type_budget.Add(DescriptorBudgetKind::SampledImage, binding.descriptorCount)",
      "type_budget.Add(DescriptorBudgetKind::StorageImage, binding.descriptorCount)",
      "type_budget.Add(DescriptorBudgetKind::Sampler, binding.descriptorCount)",
      "type_budget.Add(DescriptorBudgetKind::Unsupported, binding.descriptorCount)",
      "pipeline.descriptor_types_per_set = type_budget.Counts();",
      "pipeline.descriptor_types_known = type_budget.Known();"
    )
    if any(x not in fn for x in required):return False
    # Bindings must be traversed in the diagnostic cost loop AFTER Vulkan
    # creation, not in an earlier unrelated descriptor-count loop.
    if "DescriptorLayoutBudget type_budget;\n\tfor (const auto& binding: bindings)" not in fn:
        return False
    if fn.find("graphics.device.createDescriptorSetLayout(")>fn.find("DescriptorLayoutBudget type_budget;"):
        return False
    if s.count("CreateDescriptorLayout(graphics, pipeline, descriptor_bindings);")!=2:
        return False
    if "descriptor_types_per_set" not in p or "descriptor_types_known = false;" not in p:
        return False
    if "pipeline.descriptor_set_layout, pipeline.descriptor_types_per_set" not in b:
        return False
    if "pipeline.descriptor_types_known" not in b:return False
    if "if (pipeline.uses_push_descriptors)" not in b or "pushDescriptorSetKHR(" not in b:
        return False
    if "m_telemetry.AllocatedTyped(batch.allocation," not in h:return False
    if h.count("m_telemetry.IssuedTyped(")!=3:return False
    if "m_telemetry.DiscardTyped(discarded, discarded_types, discarded_types_known);" not in h:
        return False
    if h.find("m_telemetry.DiscardTyped(")>h.find("m_sets.clear();"):return False
    if "diagnostic_types_per_set" not in d:return False
    if s.count("#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY")<2:
        return False
    return True
class SourceBudgetContract(unittest.TestCase):
    def test_source(self):self.assertTrue(audit(*read()))
    def bad(self,n,a,b):
        source=read()
        self.assertIn(a,source[n])
        source[n]=source[n].replace(a,b,1)
        self.assertFalse(audit(*source))
    def test_wrong_origin(self):
        self.bad(0,"DescriptorLayoutBudget type_budget;\n\tfor (const auto& binding: bindings)",
                 "DescriptorLayoutBudget type_budget;\n\tfor (const auto& write: descriptor_writes)")
    def test_wrong_kind(self):
        self.bad(0,"type_budget.Add(DescriptorBudgetKind::Unsupported, binding.descriptorCount)",
                 "type_budget.Add(DescriptorBudgetKind::StorageBuffer, binding.descriptorCount)")
    def test_wrong_sampler_count(self):
        self.bad(0,"type_budget.Add(DescriptorBudgetKind::Sampler, binding.descriptorCount)",
                 "type_budget.Add(DescriptorBudgetKind::Sampler, 1)")
    def test_no_pipeline_cost(self):
        self.bad(2,"pipeline.descriptor_types_per_set","pipeline.broken_types_per_set")
    def test_missing_discard(self):
        self.bad(3,"m_telemetry.DiscardTyped(discarded, discarded_types, discarded_types_known);",
                 "m_telemetry.DiscardTyped(0, discarded_types, discarded_types_known);")
    def test_wrong_batch_metadata(self):
        self.bad(4,"diagnostic_types_per_set","broken_type_cost")
if __name__=="__main__":unittest.main()
