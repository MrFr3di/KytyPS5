import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from analyze_texture_workload_attribution import STAGES, EVENTS, parse, analyze


class AttributionSchemaTests(unittest.TestCase):
    def fixture(self, mutate=None):
        rows = (
            [dict(kind="stage", name=n, count="0", inclusive_ns="0", self_ns="0")
             for n in STAGES] +
            [dict(kind="event", name=n, count="0", inclusive_ns="0", self_ns="0")
             for n in EVENTS]
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
        self.assertEqual(r["g1_transfer_granularity"]["buffer_upload_staged_bytes_per_region"], 2048)
        self.assertEqual(r["g1_transfer_granularity"]["readback_packing_ratio"], 1.28)
        self.assertEqual(r["g1_transfer_granularity"]["image_full_upload_guest_bytes"], 8192)
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

    def test_reject_upload_batch_exceeds_regions(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: next(x for x in r if x["name"] == "buffer_upload_batches").update(count="3"))

    def test_reject_readback_packing_below_payload(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: next(x for x in r if x["name"] == "buffer_readback_packed_bytes").update(count="50"))

    def test_reject_noncanonical_unsigned(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: r[0].update(count="+2"))

    def test_reject_event_timing(self):
        with self.assertRaises(ValueError):
            self.fixture(lambda r: r[-1].update(inclusive_ns="10"))


if __name__ == "__main__":
    unittest.main()
