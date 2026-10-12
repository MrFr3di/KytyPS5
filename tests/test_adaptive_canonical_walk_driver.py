"""S4-B5-B negative validation for native-key dispatch/evidence."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from run_adaptive_canonical_walk import HEADER,NAMES,VARIANTS,parse,check_group

def row(mode):
    d={k:1 for k in HEADER[2:]}
    d.update(scenario=NAMES[0],variant=mode,repetition=0,
             owners=64,queries=192,canonical_buckets=3,owner_visits=400,
             native_checksum=999,new_requests=33,adaptive_skip=0,adaptive_find=0)
    if mode.startswith("adaptive_"):
        d.update(adaptive_skip=100,adaptive_find=92)
    return d

class DriverTests(unittest.TestCase):
    def setUp(self):self.group=[row(v) for v in VARIANTS]
    def test_good(self):check_group(self.group)
    def bad(self,n,key,value):
        self.group[n][key]=value
        with self.assertRaises(ValueError):check_group(self.group)
    def test_generation_checksum(self):self.bad(4,"native_checksum",1000)
    def test_owner_order_visit(self):self.bad(3,"owner_visits",399)
    def test_allocation_parity(self):self.bad(2,"new_requests",35)
    def test_canonical_buckets(self):self.bad(2,"canonical_buckets",4)
    def test_missing_mode(self):self.group.pop();self.assertRaises(ValueError,check_group,self.group)
    def test_wrong_dispatch_total(self):self.bad(2,"adaptive_find",1)
    def test_nonadaptive_dispatch(self):self.bad(0,"adaptive_skip",1)
    def test_short_boundary(self):
        for x in self.group:x["scenario"]="short_l1_crossing"
        self.assertRaises(ValueError,check_group,self.group)
    def test_negative_integer(self):
        values=[str(row(VARIANTS[0])[k]) for k in HEADER]
        values[HEADER.index("owners")]="-1"
        self.assertRaises(ValueError,parse,",".join(values))
    def test_extra_column(self):self.assertRaises(ValueError,parse,"a,b,c")
if __name__=="__main__":unittest.main()
