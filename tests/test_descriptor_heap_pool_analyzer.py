"""G3-D4: deterministic strict-parser falsifiers; no GPU claims."""
import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_descriptor_heap_pool import classify, parse_csv

BUCKETS=("storage_buffer","sampled_image","storage_image","sampler")
CAPACITY=(8192,8192,1024,1024)


def synthetic(typed=True):
    # Two pool epochs: 800 + 700 allocated, but peak in ANY pool = 800.
    fields=dict(
        schema_version=2, pool_creates=2,pool_resets=0,pool_retires=1,
        peak_pending_pools=1,cache_hits=0,cache_misses=2,
        allocation_attempts=2,reject_out_of_pool=0,reject_fragmented=0,
        allocated_sets=1500,issued_sets=2,discarded_unused_sets=799,
        currently_cached_unused_sets=699,peak_cached_unused_sets=800,
        pool_capacity_sets=1024,peak_pool_allocated_sets=800,
        pool_budget_known=int(typed),pool_budget_feasible=int(typed),
        type_counts_known=int(typed),gpu_timing_known=0,invariant_pass=1,
        production_allocation_policy_modified=0,
    )
    for k,cap in zip(BUCKETS,CAPACITY):
        fields[f"pool_capacity_{k}"]=cap
        fields[f"peak_pool_allocated_{k}"]=800 if typed else 0
        fields[f"allocated_{k}"]=1500 if typed else 0
        fields[f"issued_{k}"]=2 if typed else 0
        fields[f"discarded_{k}"]=799 if typed else 0
        fields[f"cached_{k}"]=699 if typed else 0
    return fields


def csv_of(fields):
    return "metric,value\n" + "".join(f"{k},{v}\n" for k,v in fields.items())


class DescriptorPoolAnalyzerTests(unittest.TestCase):
    def test_lifetime_not_capacity(self):
        m=parse_csv(csv_of(synthetic()))
        self.assertGreater(m["allocated_sets"],m["pool_capacity_sets"])
        self.assertEqual(classify(m),"CONSISTENT_DIAGNOSTIC")

    def test_single_epoch_set_breach(self):
        m=synthetic()
        m["peak_pool_allocated_sets"]=1025
        self.assertEqual(classify(m),"COUNTEREXAMPLE")

    def test_sampler_breach_despite_set_headroom(self):
        m=synthetic()
        m["peak_pool_allocated_sampler"]=1025
        self.assertEqual(classify(m),"COUNTEREXAMPLE")

    def test_unknown_types_explicitly_incomplete(self):
        self.assertEqual(classify(parse_csv(csv_of(synthetic(False)))),
                         "INSUFFICIENT_TRACE")

    def test_unknown_types_cannot_publish_estimates(self):
        m=synthetic(False)
        m["allocated_storage_buffer"]=42
        self.assertEqual(classify(m),"COUNTEREXAMPLE")

    def test_lifetime_conservation(self):
        m=synthetic()
        m["currently_cached_unused_sets"]+=1
        self.assertEqual(classify(m),"COUNTEREXAMPLE")

    def test_type_conservation(self):
        m=synthetic()
        m["issued_storage_image"]+=1
        self.assertEqual(classify(m),"COUNTEREXAMPLE")

    def test_capacity_constants_cannot_be_changed(self):
        m=synthetic()
        m["pool_capacity_storage_image"]=8192
        self.assertEqual(classify(m),"COUNTEREXAMPLE")

    def test_rejected_attempts_exceed_attempts(self):
        m=synthetic()
        m["reject_out_of_pool"]=3
        self.assertEqual(classify(m),"COUNTEREXAMPLE")

    def test_old_schema_explicitly_insufficient(self):
        m=synthetic()
        m["schema_version"]=1
        self.assertEqual(classify(m),"INSUFFICIENT_TRACE")

    def test_duplicate_key_rejected(self):
        with self.assertRaises(ValueError):
            parse_csv(csv_of(synthetic())+"allocated_sets,0\n")

    def test_negative_and_overflow_rejected(self):
        for bad in ("-1",str(1<<64),"1.0","+3"):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    parse_csv(csv_of(synthetic()).replace(
                        "allocated_sets,1500\n",f"allocated_sets,{bad}\n"))

    def test_missing_schema_metric_rejected(self):
        with self.assertRaises(ValueError):
            parse_csv(csv_of(synthetic()).replace("pool_budget_known,1\n",""))

    def test_incorrect_fps_attribution_rejected(self):
        m=synthetic()
        m["gpu_timing_known"]=1
        self.assertEqual(classify(m),"COUNTEREXAMPLE")

    def test_no_false_driver_speedup(self):
        m=synthetic()
        m["reject_fragmented"]=1
        self.assertEqual(classify(m),"CONSISTENT_DIAGNOSTIC")
        # A retry may be fragmentation/driver-specific; this CSV cannot
        # establish which pool pressure or device time caused it.


if __name__=="__main__":
    unittest.main()
