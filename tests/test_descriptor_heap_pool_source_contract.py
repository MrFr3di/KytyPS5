"""G3-D4 tripwires: runtime pool epoch reset and source pool capacities.

This is textual source shape, not a substitute for native Vulkan execution.
"""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
PRODUCER=ROOT/"src/graphics/host_gpu/renderer/pipeline/descriptorHeapTelemetry.h"
POOL=ROOT/"src/graphics/host_gpu/renderer/pipeline/descriptorHeap.cpp"


def source():
    return (PRODUCER.read_text(encoding="utf-8"),
            POOL.read_text(encoding="utf-8"))


def audit(h,c):
    gates=(
        "static constexpr uint64_t PoolSetCapacity = 1024;",
        "8192,8192,1024,1024",
        "++m_counts.created_pools;\n    StartPoolEpoch();",
        "++m_counts.reset_pools;\n    StartPoolEpoch();",
        "m_pool_current_sets += count;",
        "m_pool_peak_sets = std::max(m_pool_peak_sets, m_pool_current_sets);",
        "m_pool_current_types[i]+=charged;",
        "m_pool_peak_types[i]=std::max(m_pool_peak_types[i],m_pool_current_types[i]);",
        "m_pool_current_sets=0;",
        "m_pool_current_types={};",
        'write("schema_version", 2);',
        'write("pool_budget_known", uint64_t(PoolBudgetKnown()));',
        'write("pool_budget_feasible", uint64_t(PoolBudgetFeasible()));',
        'write("peak_pool_allocated_sets", m_pool_peak_sets);',
        '"peak_pool_allocated_%s,%llu',
    )
    if any(k not in h for k in gates):return False
    sizes=(
        "constexpr uint32_t   DescriptorHeapCount = 1024;",
        "vk::DescriptorType::eStorageBuffer, 8192",
        "vk::DescriptorType::eSampledImage, 8192",
        "vk::DescriptorType::eStorageImage, 1024",
        "vk::DescriptorType::eSampler, 1024",
    )
    if any(k not in c for k in sizes):return False
    if "create.maxSets       = DescriptorHeapCount;" not in c:return False
    if "create.pPoolSizes    = DescriptorPoolSizes.data();" not in c:return False
    if "if (m_telemetry_enabled) m_telemetry.PoolCreated();" not in c:return False
    if "if (m_telemetry_enabled) m_telemetry.PoolReset();" not in c:return False
    if "if (m_telemetry_enabled) m_telemetry.AllocateAttempt();" not in c:
        return False
    if "m_telemetry.Allocated(batch.allocation);" not in c:return False
    if "m_telemetry.AllocatedTyped(batch.allocation," not in c:return False
    # Events must be AFTER successful driver calls, never speculative attempts.
    if c.find("m_telemetry.PoolReset();")<c.find("resetDescriptorPool(m_current_pool"):
        return False
    if c.find("m_telemetry.PoolCreated();")<c.find("createDescriptorPool(&create"):
        return False
    allocation_section=c[c.find("bool DescriptorHeap::Allocate("):]
    if allocation_section.find("m_telemetry.Allocated(batch.allocation);") > allocation_section.find("return true;"):
        return False
    if "if (m_telemetry_enabled)" not in c:return False
    if "#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY" not in c:
        return False
    return True


class PoolSourceContractTests(unittest.TestCase):
    def test_source(self):self.assertTrue(audit(*source()))
    def bad(self,i,before,after):
        parts=list(source())
        self.assertIn(before,parts[i])
        parts[i]=parts[i].replace(before,after,1)
        self.assertFalse(audit(*parts))
    def test_pool_maxset_not_drifted(self):
        self.bad(1,"constexpr uint32_t   DescriptorHeapCount = 1024;",
                 "constexpr uint32_t   DescriptorHeapCount = 2048;")
    def test_sampler_capacity_not_drifted(self):
        self.bad(1,"vk::DescriptorType::eSampler, 1024",
                 "vk::DescriptorType::eSampler, 8192")
    def test_no_epoch_reset_on_pool_reuse(self):
        self.bad(0,"++m_counts.reset_pools;\n    StartPoolEpoch();",
                 "++m_counts.reset_pools;\n    // missing epoch start")
    def test_no_epoch_reset_on_new_pool(self):
        self.bad(0,"++m_counts.created_pools;\n    StartPoolEpoch();",
                 "++m_counts.created_pools;\n    // missing epoch start")
    def test_missing_peak_tracking(self):
        self.bad(0,"m_pool_current_sets += count;","m_pool_current_sets += 0;")
    def test_unsourced_capacity_change(self):
        self.bad(0,"8192,8192,1024,1024","8192,8192,1024,8192")
    def test_schema_is_versioned(self):
        self.bad(0,'write("schema_version", 2);',
                 'write("schema_version", 1);')


if __name__=="__main__":
    unittest.main()
