"""Negative parser tests for actual static COFF disassembly evidence."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from analyze_shortpath_codegen import SYMBOLS,verify
class Tests(unittest.TestCase):
    def text(self):
        return "Microsoft COFF disassembly\n"+ "\n".join(
            f"00000000 <{s}>:\n  0: 48 8b 0a mov rax,[rdx]" for s in SYMBOLS
        )*3
    def test_valid(self):self.assertEqual(len(verify(self.text())),4)
    def test_lost_symbol(self):
        with self.assertRaises(ValueError):
            verify(self.text().replace("KytyB5DPageOnly256","Corrupted"))
    def test_short(self):
        with self.assertRaises(ValueError):verify("x")
    def test_unicode(self):
        with self.assertRaises(ValueError):verify(self.text()+"\u00e9")
if __name__=="__main__":unittest.main()
