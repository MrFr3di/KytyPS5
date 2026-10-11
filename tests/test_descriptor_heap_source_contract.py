"""G3-D0 production descriptor heap textual source guard, NOT a Vulkan proof."""
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/graphics/host_gpu/renderer/pipeline/descriptorHeap.h"
CPP = ROOT / "src/graphics/host_gpu/renderer/pipeline/descriptorHeap.cpp"
RENDER = ROOT / "src/graphics/host_gpu/renderer/pipeline/descriptors.cpp"

def accepts(h, c, r):
    if "DescriptorSetBatch = 32;" not in h:
        return False
    for marker in ("DescriptorHeapCount = 1024;", "eStorageBuffer, 8192}",
                   "eSampledImage, 8192}", "eStorageImage, 1024}",
                   "eSampler, 1024}", "m_master_semaphore.Wait(tick);",
                   "create.maxSets       = DescriptorHeapCount;"):
        if marker not in c:
            return False
    start = c.find("vk::DescriptorSet DescriptorHeap::Commit(")
    end = c.find("void DescriptorHeap::CreateDescriptorPool()", start)
    if start < 0 or end <= start:
        return False
    body = c[start:end]
    position = -1
    for marker in ("auto& batch = m_sets[layout];", "if (batch.size != 0)",
                   "if (Allocate(layout, batch))",
                   "m_pending_pools.emplace_back(m_current_pool, m_master_semaphore.CurrentTick());",
                   "m_master_semaphore.IsFree(tick)", "resetDescriptorPool(m_current_pool, {})",
                   "CreateDescriptorPool();", "m_sets.clear();",
                   "Allocate(layout, fresh_batch)", "bool DescriptorHeap::Allocate(",
                   "layouts.fill(layout);", "allocate.descriptorSetCount = batch.allocation;",
                   "allocateDescriptorSets(&allocate, batch.sets.data())",
                   "vk::Result::eErrorOutOfPoolMemory",
                   "vk::Result::eErrorFragmentedPool", "batch.allocation /= 2;"):
        position = body.find(marker, position+1)
        if position < 0:
            return False
    begin = r.find("void RenderExecutor::CommitBindings(")
    if begin < 0:
        return False
    for marker in ("pipeline.uses_push_descriptors", "pushDescriptorSetKHR(",
                   "GetDescriptorHeap().Commit(pipeline.descriptor_set_layout)",
                   "device.updateDescriptorSets(", "vk_buffer.bindDescriptorSets("):
        if marker not in r[begin:]:
            return False
    return True

def read():
    return [p.read_text(encoding="utf-8") for p in (HEADER, CPP, RENDER)]

class DescriptorSourceTests(unittest.TestCase):
    def test_source(self):
        self.assertTrue(accepts(*read()))
    def bad(self, index, needle, mutation):
        sources = read()
        self.assertIn(needle, sources[index])
        sources[index] = sources[index].replace(needle, mutation, 1)
        self.assertFalse(accepts(*sources))
    def test_batch_size(self):
        self.bad(0, "DescriptorSetBatch = 32;", "DescriptorSetBatch = 64;")
    def test_capacity(self):
        self.bad(1, "eStorageImage, 1024}", "eStorageImage, 512}")
    def test_unsafe_early_reset(self):
        self.bad(1, "m_master_semaphore.IsFree(tick)", "true")
    def test_missing_layout_invalidation(self):
        self.bad(1, "m_sets.clear();", "m_sets.size();")
    def test_halving_removed(self):
        self.bad(1, "batch.allocation /= 2;", "batch.allocation = 1;")
    def test_push_descriptors_disabled(self):
        self.bad(2, "pushDescriptorSetKHR(", "invalidPushDescriptor(")
    def test_heap_path_disabled(self):
        self.bad(2, "GetDescriptorHeap().Commit(pipeline.descriptor_set_layout)",
                 "GetDescriptorHeap().WrongCommit(pipeline.descriptor_set_layout)")
if __name__ == "__main__":
    unittest.main()
