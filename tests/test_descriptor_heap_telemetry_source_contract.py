"""G3-D2 real DescriptorHeap compile/runtime gate and event ordering tripwires.

Source-shape regressions only; do not imply Vulkan-driver/runtime coverage.
"""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
CPP=ROOT/"src/graphics/host_gpu/renderer/pipeline/descriptorHeap.cpp"
HEADER=ROOT/"src/graphics/host_gpu/renderer/pipeline/descriptorHeap.h"
CMAKE=ROOT/"CMakeLists.txt"

def audit(c,h,m):
    guard="#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY"
    if h.count(guard)<1 or c.count(guard)<8:
        return False
    if "m_telemetry_enabled = trace != nullptr && std::strcmp(trace, \"1\") == 0;" not in c:
        return False
    if "KYTY_DESCRIPTOR_HEAP_TRACE" not in c:
        return False
    if "descriptor-heap-telemetry.csv" not in c:
        return False
    if "if (m_telemetry_enabled)" not in c:
        return False
    if "option(KYTY_DESCRIPTOR_HEAP_TELEMETRY" not in m:
        return False
    if "Optional descriptor heap telemetry\" OFF)" not in m:
        return False
    if "target_compile_definitions(kyty_emulator PRIVATE KYTY_DESCRIPTOR_HEAP_TELEMETRY=1)" not in m:
        return False
    if "DescriptorHeapTelemetry m_telemetry;" not in h:
        return False
    checks=(
        ("vk::DescriptorSet DescriptorHeap::Commit(", "m_telemetry.CacheHit();", "return batch.sets[--batch.size];"),
        ("vk::DescriptorSet DescriptorHeap::Commit(", "m_telemetry.CacheMiss();", "if (Allocate(layout, batch))"),
        ("vk::DescriptorSet DescriptorHeap::Commit(", "m_telemetry.PoolRetired(m_pending_pools.size());", "m_master_semaphore.IsFree(tick)"),
        ("vk::DescriptorSet DescriptorHeap::Commit(", "m_telemetry.DiscardUnused(discarded);", "m_sets.clear();"),
        ("bool DescriptorHeap::Allocate(", "m_telemetry.AllocateAttempt();", "allocateDescriptorSets(&allocate, batch.sets.data())"),
        ("bool DescriptorHeap::Allocate(", "m_telemetry.Allocated(batch.allocation);", "return true;"),
        ("bool DescriptorHeap::Allocate(", "m_telemetry.AllocateRejected(", "batch.allocation /= 2;"),
        ("void DescriptorHeap::CreateDescriptorPool(", "m_telemetry.PoolCreated();", "}"),
    )
    for (begin,x,y) in checks:
        section=c.find(begin)
        if section<0:
            return False
        i=c.find(x,section)
        j=c.find(y,section)
        if i<0 or j<0 or j<i or (begin.startswith("vk::") and i>c.find("bool DescriptorHeap::Allocate(",section)):
            return False
    if c.count("m_telemetry.Issued();")!=3:
        return False
    return True

def sources():
    return (CPP.read_text(encoding="utf8"),HEADER.read_text(encoding="utf8"),
            CMAKE.read_text(encoding="utf8"))

class TelemetryIntegrationTripwires(unittest.TestCase):
    def test_source(self): self.assertTrue(audit(*sources()))
    def mutate(self,n,a,b):
        parts=list(sources())
        self.assertIn(a,parts[n])
        parts[n]=parts[n].replace(a,b,1)
        self.assertFalse(audit(*parts))
    def test_runtime_literal(self):
        self.mutate(0,'std::strcmp(trace, "1") == 0','std::strcmp(trace, "0") == 0')
    def test_missing_retirement(self):
        self.mutate(0,"m_telemetry.PoolRetired(m_pending_pools.size());",
                    "m_telemetry.PoolRetired(0);")
    def test_missing_discard(self):
        self.mutate(0,"m_telemetry.DiscardUnused(discarded);",
                    "m_telemetry.DiscardUnused(0);")
    def test_missing_attempt(self):
        self.mutate(0,"m_telemetry.AllocateAttempt();","m_telemetry.CacheHit();")
    def test_missing_issued(self):
        self.mutate(0,"m_telemetry.Issued();","m_telemetry.CacheHit();")
    def test_unprotected_option(self):
        self.mutate(2,"Optional descriptor heap telemetry\" OFF)",
                    "Optional descriptor heap telemetry\" ON)")
    def test_missing_file_guard(self):
        self.mutate(0,'descriptor-heap-telemetry.csv','descriptor-heap-always.csv')
if __name__=="__main__":
    unittest.main()
