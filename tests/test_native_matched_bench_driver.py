"""Negative controls for S4-B4b native SlotId benchmark output gate."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from run_native_matched_bench import COLUMNS,parse_line,verify_pair,VARIANTS,SCENARIOS

def example(variant):
    row={key:1 for key in COLUMNS[2:]}
    row.update(scenario=SCENARIOS[0],variant=variant,repetition=0,
               owners=64,queries=648,native_checksum=314159,
               canonical_buckets=1,new_calls=100,delete_calls=90,
               peak_live_bytes=70,requested_total_bytes=1000,
               live_after_register_bytes=60,live_after_query_bytes=60)
    row.update(sidecar_answered=40 if variant==VARIANTS[1] else 0,
               canonical_fallbacks=30 if variant==VARIANTS[1] else 0,
               snapshot_builds=2 if variant==VARIANTS[1] else 0,
               sidecar_nominal_payload_bytes=400 if variant==VARIANTS[1] else 0,
               has_sidecar=int(variant==VARIANTS[1]))
    return row

class NativeMatchedHarnessTests(unittest.TestCase):
    def setUp(self):
        self.a=example(VARIANTS[0]);self.b=example(VARIANTS[1])
    def test_valid(self):verify_pair(self.a,self.b)
    def bad(self,key,val):
        self.b[key]=val
        with self.assertRaises(ValueError):verify_pair(self.a,self.b)
    def test_native_generation_checksum(self):self.bad("native_checksum",456789)
    def test_missing_fallback(self):self.bad("canonical_fallbacks",0)
    def test_missing_rebuild(self):self.bad("snapshot_builds",1)
    def test_wrong_bucket_cost(self):self.bad("canonical_buckets",3)
    def test_more_deletes_than_allocations(self):self.bad("delete_calls",110)
    def test_registration_peak_underflow(self):self.bad("peak_live_bytes",1)
    def test_wrong_query_count(self):self.bad("queries",647)
    def test_missing_sidecar_identity(self):self.bad("has_sidecar",0)
    def test_malformed_csv(self):
        with self.assertRaises(ValueError):parse_line("canonical_only,0")
    def test_negative_count(self):
        items=[str(self.a[c]) for c in COLUMNS]
        items[COLUMNS.index("owners")]="-1"
        with self.assertRaises(ValueError):parse_line(",".join(items))

if __name__=="__main__":unittest.main()
