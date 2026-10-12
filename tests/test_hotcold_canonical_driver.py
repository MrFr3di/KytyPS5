"""S4-B5-E negative checks for native key/parity and cold-path dispatch."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from run_hotcold_canonical import HEADER,NAMES,VARIANTS,BATCH,parse,check_group
def row(mode,scenario=NAMES[3]):
    d={k:1 for k in HEADER[2:]}
    d.update(scenario=scenario,variant=mode,repetition=0,owners=16,
        queries=192*BATCH[NAMES.index(scenario)],batch=BATCH[NAMES.index(scenario)],
        native_checksum=12345,canonical_buckets=2,owner_visits=500,new_requests=40,
        adaptive_skip=0,adaptive_find=0)
    if mode in VARIANTS[1:5]:
        if scenario==NAMES[4]:d["adaptive_skip"]=d["queries"]
        else:d["adaptive_find"]=d["queries"]
    return d
class Tests(unittest.TestCase):
    def setUp(self):self.group=[row(v) for v in VARIANTS]
    def test_valid(self):check_group(self.group)
    def mutated(self,i,key,value):
        self.group[i][key]=value
        with self.assertRaises(ValueError):check_group(self.group)
    def test_checksum(self):self.mutated(2,"native_checksum",12346)
    def test_allocator(self):self.mutated(2,"new_requests",41)
    def test_visit(self):self.mutated(2,"owner_visits",501)
    def test_bucket(self):self.mutated(2,"canonical_buckets",3)
    def test_double_select(self):self.mutated(2,"adaptive_skip",1)
    def test_wrong_false_branch(self):self.mutated(2,"adaptive_find",0)
    def test_wrong_batch(self):self.mutated(2,"batch",1)
    def test_no_cold_arm(self):
        self.group=[row(v,NAMES[4]) for v in VARIANTS]
        self.mutated(2,"adaptive_skip",0)
    def test_missing_mode(self):
        self.group.pop()
        self.assertRaises(ValueError,check_group,self.group)
    def test_invalid_signed(self):
        x=[str(self.group[0][k]) for k in HEADER]
        x[HEADER.index("owners")]="-2"
        self.assertRaises(ValueError,parse,",".join(x))
    def test_shape(self):self.assertRaises(ValueError,parse,"junk")
if __name__=="__main__":unittest.main()
