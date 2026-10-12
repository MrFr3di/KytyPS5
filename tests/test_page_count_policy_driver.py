"""S4-B5-C negative validation of native provenance and policy dispatch."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from run_page_count_policy import HEADER,NAMES,VARIANTS,BATCH,parse,check_group
def sample(variant,scenario=NAMES[0]):
    d={k:1 for k in HEADER[2:]}
    d.update(scenario=scenario,variant=variant,repetition=0,owners=32,
             queries=192*BATCH[NAMES.index(scenario)],batch=BATCH[NAMES.index(scenario)],
             native_checksum=123456,canonical_buckets=5,owner_visits=2048,new_requests=55,
             adaptive_skip=0,adaptive_find=0)
    if variant in VARIANTS[2:]:d["adaptive_find"]=d["queries"]
    return d
class DriverTests(unittest.TestCase):
    def setUp(self):self.g=[sample(v) for v in VARIANTS]
    def bad(self,n,k,v):
        self.g[n][k]=v
        with self.assertRaises(ValueError):check_group(self.g)
    def test_valid(self):check_group(self.g)
    def test_wrong_checksum(self):self.bad(4,"native_checksum",123457)
    def test_lost_native_visits(self):self.bad(4,"owner_visits",2000)
    def test_allocator(self):self.bad(3,"new_requests",56)
    def test_bucket_count(self):self.bad(4,"canonical_buckets",6)
    def test_bad_dispatch_sum(self):self.bad(4,"adaptive_find",1)
    def test_batch_drift(self):self.bad(4,"batch",1)
    def test_wrong_mode(self):self.g.pop();self.assertRaises(ValueError,check_group,self.g)
    def test_short_skip(self):
        self.g=[sample(v,NAMES[4]) for v in VARIANTS]
        self.bad(4,"adaptive_skip",1)
    def test_wrong_single_bucket(self):
        self.g=[sample(v,NAMES[5]) for v in VARIANTS]
        self.assertRaises(ValueError,check_group,self.g)
    def test_bad_field(self):
        vals=[str(self.g[0][k]) for k in HEADER]
        vals[HEADER.index("owners")]="-1"
        self.assertRaises(ValueError,parse,",".join(vals))
    def test_bad_length(self):self.assertRaises(ValueError,parse,"bad,record")
if __name__=="__main__":unittest.main()
