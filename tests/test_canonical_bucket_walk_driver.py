"""S4-B5-A negative parse/semantic tests for canonical lookup comparison."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from run_canonical_bucket_walk import HEADER,NAMES,VARIANTS,parse,check_pair

def good(variant):
    obj={k:1 for k in HEADER[2:]}
    obj.update(scenario=NAMES[0],variant=variant,repetition=0,owners=64,
               queries=192,canonical_buckets=1,owner_visits=120,
               native_checksum=123456,new_requests=456)
    return obj

class TestCanonicalBucketWalk(unittest.TestCase):
    def setUp(self):
        self.a=good(VARIANTS[0]);self.b=good(VARIANTS[1])
    def test_valid(self):check_pair(self.a,self.b)
    def mutate(self,key,new):
        self.b[key]=new
        with self.assertRaises(ValueError):check_pair(self.a,self.b)
    def test_slot_generation_checksum(self):self.mutate("native_checksum",123457)
    def test_owner_visit_semantics(self):self.mutate("owner_visits",121)
    def test_canonical_storage_parity(self):self.mutate("canonical_buckets",2)
    def test_no_extra_index_allocations(self):self.mutate("new_requests",500)
    def test_wrong_query_count(self):self.mutate("queries",193)
    def test_wrong_scenario(self):self.b["scenario"]=NAMES[1];self.assertRaises(ValueError,check_pair,self.a,self.b)
    def test_negative(self):
        values=[str(self.a[k]) for k in HEADER]
        values[HEADER.index("owners")]="-1"
        self.assertRaises(ValueError,parse,",".join(values))
    def test_overlength(self):self.assertRaises(ValueError,parse,"abc,def")
if __name__=="__main__":unittest.main()
