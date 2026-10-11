"""G1-B1 source-change guard, NOT a C++ semantic / GPU synchronization proof."""
import unittest
from pathlib import Path

FILE = Path(__file__).resolve().parents[1] / "src/graphics/host_gpu/renderer/cache/bufferCache.cpp"

def production_readback_contract(source: str) -> bool:
    begin = source.find("bool BufferCache::DownloadBufferMemory(")
    end = source.find("bool BufferCache::SynchronizeBuffer(", begin)
    if begin < 0 or end <= begin:
        return False
    body = source[begin:end]
    markers = [
        "m_gpu_modified_ranges.ForEachInRange(address, bytes",
        "copies.emplace_back(start - buffer_address, total_size, end - start)",
        "total_size += Common::AlignUp(end - start, 64)",
        "BufferReadbackPackedBytes, total_size",
        "BufferReadbackRegions, copies.size()",
        "m_download_buffer.Map(total_size, 64)",
        "native.copyBuffer(buffer.Handle(), download.Handle()",
    ]
    pos = -1
    for marker in markers:
        i = body.find(marker, pos + 1)
        if i < 0:
            return False
        pos = i
    return True

class ProductionTransferSourceContractTests(unittest.TestCase):
    def test_real_checked_out_source(self):
        self.assertTrue(production_readback_contract(FILE.read_text(encoding="utf-8")))

    def test_reject_wrong_alignment(self):
        src = FILE.read_text(encoding="utf-8")
        self.assertIn("Common::AlignUp(end - start, 64)", src)
        self.assertFalse(production_readback_contract(
            src.replace("Common::AlignUp(end - start, 64)", "Common::AlignUp(end - start, 32)", 1)))

    def test_reject_staging_source_offset(self):
        src = FILE.read_text(encoding="utf-8")
        self.assertIn("copies.emplace_back(start - buffer_address, total_size, end - start)", src)
        self.assertFalse(production_readback_contract(
            src.replace("copies.emplace_back(start - buffer_address, total_size, end - start)",
                        "copies.emplace_back(total_size, start - buffer_address, end - start)", 1)))

    def test_reject_copy_removal(self):
        src = FILE.read_text(encoding="utf-8")
        self.assertIn("native.copyBuffer(buffer.Handle(), download.Handle()", src)
        self.assertFalse(production_readback_contract(
            src.replace("native.copyBuffer(buffer.Handle(), download.Handle()",
                        "native.invalidCopy(buffer.Handle(), download.Handle()", 1)))

if __name__ == "__main__":
    unittest.main()
