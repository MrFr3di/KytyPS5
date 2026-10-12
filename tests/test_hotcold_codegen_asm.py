"""Parser negative fixtures: missing COFF sections/probes may not pass."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from analyze_hotcold_codegen import SYMBOLS,verify
class Tests(unittest.TestCase):
    def fixture(self):
        return "sample optimized COFF machine disassembly\n"*10 + "".join(
            f"00000000 <{sym}>:\n   0: 48 8b 0a\tmovq (%rdx), %rcx\n"
            f"   3: c3\tretq\n" for sym in SYMBOLS)
    def test_valid(self):
        m=verify(self.fixture())
        self.assertEqual(len(m),5)
        self.assertEqual(m["KytyB5EHotCold256"],(2,4))
    def test_missing_symbol(self):
        with self.assertRaises(ValueError):
            verify(self.fixture().replace("KytyB5EColdSkip","FakeSymbol"))
    def test_no_instruction(self):
        self.assertEqual(verify(self.fixture().replace("   0: 48 8b 0a","   0: ???").replace("   3: c3","   3: ???")),{})
    def test_short(self):
        with self.assertRaises(ValueError):verify("empty")
    def test_unicode(self):
        with self.assertRaises(ValueError):verify(self.fixture()+"é")
if __name__=="__main__":unittest.main()
