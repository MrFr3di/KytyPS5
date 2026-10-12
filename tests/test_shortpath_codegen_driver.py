"""S4-B5-D matched native evidence negative validation."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from run_shortpath_codegen import HEADER,NAMES,VARIANTS,BATCH,parse,check_group

def row(mode,sc=NAMES[2]):
    vals={k:1 for k in HEADER[2:]}
    vals.update(scenario=sc,variant=mode,repetition=0,owners=16,
          queries=192*BATCH[NAMES.index(sc)],batch=BATCH[NAMES.index(sc)],
          native_checksum=123456,canonical_buckets=2,owner_visits=500,
          new_requests=30,adaptive_skip=0,adaptive_find=0)
    if mode in VARIANTS[2:5]:vals["adaptive_find"]=vals["queries"]
    return vals
class Tests(unittest.TestCase):
    def setUp(self):self.rows=[row(mode) for mode in VARIANTS]
    def test_valid(self):check_group(self.rows)
    def reject(self,i,k,v):
        self.rows[i][k]=v
        with self.assertRaises(ValueError):check_group(self.rows)
    def test_wrong_native_generation_checksum(self):self.reject(2,"native_checksum",123457)
    def test_wrong_owner_visit_count(self):self.reject(2,"owner_visits",501)
    def test_extra_allocation(self):self.reject(2,"new_requests",40)
    def test_bucket_drift(self):self.reject(2,"canonical_buckets",3)
    def test_wrong_dispatch_count(self):self.reject(2,"adaptive_find",1)
    def test_short_skip(self):self.reject(2,"adaptive_skip",1)
    def test_no_mode(self):self.rows.pop();self.assertRaises(ValueError,check_group,self.rows)
    def test_wrong_batch(self):self.reject(2,"batch",1)
    def test_wrong_long_policy(self):
        self.rows=[row(m,NAMES[4]) for m in VARIANTS]
        self.assertRaises(ValueError,check_group,self.rows)
    def test_bad_shape(self):self.assertRaises(ValueError,parse,"nonsense")
    def test_bad_unsigned(self):
        vals=[str(self.rows[0][k]) for k in HEADER]
        vals[HEADER.index("owners")]="-1"
        self.assertRaises(ValueError,parse,",".join(vals))
if __name__=="__main__":unittest.main()
