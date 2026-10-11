import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from analyze_texture_workload_attribution import STAGES, EVENTS, GAUGES, parse, analyze


class AttributionSchemaTests(unittest.TestCase):
    def fixture(self, mutate=None):
        rows = (
            [dict(kind="stage", name=n, count="0", inclusive_ns="0", self_ns="0")
             for n in STAGES] +
            [dict(kind="event", name=n, count="0", inclusive_ns="0", self_ns="0")
             for n in EVENTS] +
            [dict(kind="gauge", name=n, count="0", inclusive_ns="0", self_ns="0")
             for n in GAUGES]
        )
        rows[0].update(count="2", inclusive_ns="100", self_ns="60")
        rows[1].update(count="1", inclusive_ns="30", self_ns="20")
        for row in rows:
            if row["name"] == "rebind_reuse":
                row["count"] = "3"
            if row["name"] == "rebind_needed":
                row["count"] = "1"
            if row["name"] == "buffer_upload_staged_bytes":
                row["count"] = "4096"
            if row["name"] == "buffer_upload_regions":
                row["count"] = "2"
            if row["name"] == "buffer_upload_batches":
                row["count"] = "1"
            if row["name"] == "buffer_readback_payload_bytes":
                row["count"] = "100"
            if row["name"] == "buffer_readback_packed_bytes":
                row["count"] = "128"
            if row["name"] == "buffer_readback_regions":
                row["count"] = "1"
            if row["name"] == "image_upload_requests":
                row["count"] = "1"
            if row["name"] == "image_upload_guest_bytes":
                row["count"] = "8192"
            resource = {
                "resource_snapshots": 2,
                "image_gc_checks": 10, "image_gc_below_trigger": 8,
                "image_gc_root_candidates": 4, "image_gc_root_retire_requests": 1,
                "image_slots_live": 5, "image_slots_capacity": 16,
                "image_slots_registered": 3, "native_images_live": 4,
                "image_views_live": 9, "image_slots_peak_observed": 12,
                "image_views_peak_observed": 25,
            }
            if row["name"] in resource:
                row["count"] = str(resource[row["name"]])
        if mutate:
            mutate(rows)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "attribution.csv"
            with p.open("w", newline="", encoding="utf-8") as f:
                writer = csv.DictWriter(f, fieldnames=["kind", "name", "count",
                                                     "inclusive_ns", "self_ns"])
                writer.writeheader()
                writer.writerows(rows)
            return parse(p)

    def test_valid_with_no_go(self):
        r = analyze(self.fixture())
        self.assertEqual(r["recorded_instrumented_self_wall_ns"], 80)
        self.assertEqual(r["region_lookup_share_of_instrumented_self"], 0.75)
        self.assertEqual(r["rebind_reuse_share"], 0.75)
        self.assertEqual(r["transfer_volume_counters"]["buffer_upload_staged_bytes"], 4096)
        self.assertEqual(r["g1_transfer_granularity"]["buffer_upload_regions"], 2)
        self.assertEqual(r["g1_transfer_granularity"]["buffer_upload_tracker_page_instances"], 1)
        self.assertEqual(r["g1_transfer_granularity"]["buffer_upload_staged_bytes_per_region"], 2048)
        self.assertEqual(r["g1_transfer_granularity"]["readback_packing_ratio"], 1.28)
        self.assertEqual(r["g1_transfer_granularity"]["image_full_upload_guest_bytes"], 8192)
        self.assertEqual(r["g2_resource_lifecycle"]["image_views_peak_observed"], 25)
        self.assertEqual(r["g2_resource_lifecycle"]["image_gc_below_trigger_share"], 0.8)
        self.assertEqual(r["decision"], "INSUFFICIENT_FOR_PRODUCTION_GO")

    def test_reject_self_larger_than_inclusive(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: r[0].update(self_ns="101"))

    def test_reject_missing_stage(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: r.pop(0))

    def test_reject_duplicate_metric(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: r.append(r[0].copy()))

    def test_reject_negative_unsigned(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: r[0].update(count="-1"))

    def test_empty_stage_trace_has_no_top_bottleneck(self):
        def clear_stages(rows):
            for row in rows:
                if row["kind"] == "stage":
                    row.update(count="0", inclusive_ns="0", self_ns="0")
        report = analyze(self.fixture(clear_stages))
        self.assertEqual(report["top_stages"], [])
        self.assertIsNone(report["region_lookup_share_of_instrumented_self"])

    def test_reject_non_page_aligned_upload(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: next(x for x in r if x["name"] == "buffer_upload_staged_bytes").update(count="4097"))

    def test_reject_upload_batch_exceeds_regions(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: next(x for x in r if x["name"] == "buffer_upload_batches").update(count="3"))

    def test_reject_readback_packing_below_payload(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: next(x for x in r if x["name"] == "buffer_readback_packed_bytes").update(count="50"))

    def test_reject_noncanonical_unsigned(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: r[0].update(count="+2"))

    def test_reject_missing_gauge(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda rows: rows.pop(-1))

    def test_reject_invalid_slot_occupancy(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda rows: next(r for r in rows if r["name"] == "image_slots_registered").update(count="25"))

    def test_reject_gc_return_greater_than_checks(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda rows: next(r for r in rows if r["name"] == "image_gc_below_trigger").update(count="11"))

    def test_reject_observed_peak_lower_than_snapshot(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda rows: next(r for r in rows if r["name"] == "image_views_peak_observed").update(count="1"))

    def test_reject_missing_resource_snapshot(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda rows: next(r for r in rows if r["name"] == "resource_snapshots").update(count="0"))

    def test_reject_event_timing(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: r[-1].update(inclusive_ns="10"))


if __name__ == "__main__":
    unittest.main()
