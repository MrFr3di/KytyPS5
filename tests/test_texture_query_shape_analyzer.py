"""S4-B5-F: strict shape consistency and provenance negatives."""
import tempfile
import unittest
from pathlib import Path
from scripts.analyze_texture_query_shape import SHAPES, analyze, parse
from test_texture_range_telemetry_analyzer import data as legacy_fixture

def sample():
    v=legacy_fixture()
    # Existing legacy fixture: 20 queries in 1-page and 8-15-page bins.
    v.update({key:0 for key in SHAPES})
    v["shape_queries"]=20
    v["shape_l1_cross_queries"]=3
    return v

class Tests(unittest.TestCase):
    def test_unverified(self):
        got=analyze(sample())
        self.assertEqual(got["observed_valid_region_lookups"],20)
        self.assertEqual(got["page256plus_queries"],0)
        self.assertEqual(got["source_classification"],"UNVERIFIED_SOURCE")
        self.assertEqual(got["production_go"],"BLOCKED_NO_REPRESENTATIVE_GAME_TRACE")
        self.assertIsNone(analyze(dict.fromkeys(SHAPES,0) |
            {k:0 for k in legacy_fixture()})["page256plus_share"])
    def test_explicit_fixture(self):
        self.assertEqual(analyze(sample(),True)["source_classification"],
                         "SYNTHETIC_CI_FIXTURE")
    def test_missing_counter(self):
        v=sample()
        del v["shape_queries"]
        with self.assertRaisesRegex(ValueError,"shape_queries"):
            analyze(v)
    def test_invalid_counter_type(self):
        for bad in (-1,2**64,True,"12"):
            with self.subTest(value=bad):
                v=sample()
                v["shape_queries"]=bad
                with self.assertRaises(ValueError):analyze(v)
    def test_consistency_mutants(self):
        mutations={
            "shape_queries":19,
            "shape_l1_cross_queries":21,
            "shape_pages_64plus":1,
            "shape_pages_256plus":1,
            "shape_pages_1024plus":1,
            "shape_pages_256plus_same_l1":1,
            "shape_pages_256plus_cross_l1":1,
        }
        for key,value in mutations.items():
            with self.subTest(field=key):
                v=sample();v[key]=value
                with self.assertRaises(ValueError):analyze(v)
    def test_actual_256_partition(self):
        v=sample()
        v["query_pages_1"]=15
        v["query_pages_8_15"]=0
        v["query_pages_256_plus"]=5
        v["queried_pages"]=1500
        v["query_ns_pages_1"]=150
        v["query_ns_pages_8_15"]=0
        v["query_ns_pages_256_plus"]=50
        v["current_generation_large_queries"]=5
        v["current_generation_queries"]=20
        v["completed_generation_large_queries"]=0
        v["completed_generation_large_queries_after_eight"]=0
        v["generations_with_eight_large_queries"]=0
        v["max_large_queries_per_completed_generation"]=0
        v["generation_large_queries_8_15"]=0
        v["generation_large_queries_0"]=3
        v.update(shape_l1_cross_queries=3,shape_pages_64plus=5,
                 shape_pages_256plus=5,shape_pages_1024plus=2,
                 shape_pages_256plus_same_l1=3,shape_pages_256plus_cross_l1=2)
        self.assertEqual(analyze(v)["page256plus_same_l1_queries"],3)
        v["shape_pages_256plus_cross_l1"]=4
        with self.assertRaises(ValueError):analyze(v)
    def test_page_lower_upper_bounds(self):
        v=sample()
        v["queried_pages"]=20
        with self.assertRaisesRegex(ValueError,"lower bound"):analyze(v)
        v["queried_pages"]=10_000
        with self.assertRaisesRegex(ValueError,"upper bound"):analyze(v)
    def test_csv_duplicates_and_missing(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"fixture.csv"
            v=sample()
            p.write_text("metric,value\n"+"".join(f"{k},{x}\n" for k,x in v.items()),
                         encoding="utf-8")
            self.assertEqual(parse(p),v)
            p.write_text(p.read_text()+"shape_queries,99\n",encoding="utf-8")
            with self.assertRaises(ValueError):parse(p)
            del v["shape_queries"]
            p.write_text("metric,value\n"+"".join(f"{k},{x}\n" for k,x in v.items()),
                         encoding="utf-8")
            with self.assertRaisesRegex(ValueError,"shape_queries"):parse(p)
if __name__=="__main__":
    unittest.main()
