"""Stdlib-only tests for scripts/analyze_texture_range_telemetry.py."""
import tempfile
import unittest
from pathlib import Path

from scripts.analyze_texture_range_telemetry import BINS, GEN_BINS, analyze, parse


def data():
    base = dict.fromkeys([
        "queries", "invalid_queries", "page_overlap_queries", "owners_inspected",
        "results", "queried_pages", "query_ns", "registrations", "unregistrations",
        "registered_pages", "unregistered_pages", "active_images_at_dump",
        "max_active_images", "completed_generations", "generations_with_queries",
        "generations_with_eight_large_queries",
        "max_large_queries_per_completed_generation",
        "completed_generation_large_queries",
        "completed_generation_large_queries_after_eight",
        "current_generation_queries", "current_generation_large_queries",
    ], 0)
    base.update({f"generation_large_queries_{b}": 0 for b in GEN_BINS})
    base.update({f"query_pages_{b}": 0 for b in BINS})
    base.update({f"query_ns_pages_{b}": 0 for b in BINS})
    base.update({
        "queries": 20, "queried_pages": 100, "query_ns": 200,
        "owners_inspected": 40, "results": 4, "completed_generations": 3,
        "generations_with_queries": 2, "generations_with_eight_large_queries": 1,
        "max_large_queries_per_completed_generation": 10,
        "completed_generation_large_queries": 10,
        "completed_generation_large_queries_after_eight": 2,
        "current_generation_queries": 1, "current_generation_large_queries": 1,
        "generation_large_queries_0": 2, "generation_large_queries_8_15": 1,
        "query_pages_1": 9, "query_pages_8_15": 11,
        "query_ns_pages_1": 90, "query_ns_pages_8_15": 110,
    })
    return base


class TelemetryAnalyzerTests(unittest.TestCase):
    def test_measured_index_share_is_not_descriptor_cache_ratio(self):
        summary = analyze(data())
        self.assertEqual(summary["index_queries"], 20)
        self.assertAlmostEqual(summary["large_query_share"], 0.55)
        self.assertEqual(summary["mean_large_query_ns"], 10.0)
        self.assertEqual(summary["completed_generation_large_queries_after_eight"], 2)
        self.assertEqual(summary["qualifying_generation_share"], 0.5)
        self.assertEqual(summary["decision"], "INSUFFICIENT_FOR_PRODUCTION_GO")
        self.assertIn("descriptor-cache", summary["limitations"][0])

    def test_invalid_invariants_rejected(self):
        cases = [
            ("queries", 21),
            ("query_ns", 201),
            ("results", 41),
            ("page_overlap_queries", 21),
            ("generations_with_eight_large_queries", 3),
            ("current_generation_large_queries", 5),
            ("completed_generation_large_queries", 11),
            ("generation_large_queries_0", 1),
            ("completed_generation_large_queries_after_eight", 11),
        ]
        for field, value in cases:
            with self.subTest(field=field):
                sample = data()
                sample[field] = value
                with self.assertRaises(ValueError):
                    analyze(sample)

    def test_strict_csv_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.csv"
            sample = data()
            path.write_text("metric,value\n" + "".join(
                f"{key},{value}\n" for key, value in sample.items()
            ), encoding="utf-8")
            self.assertEqual(parse(path), sample)
            path.write_text(path.read_text(encoding="utf-8") + "queries,5\n",
                            encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate"):
                parse(path)
            path.write_text("metric,value\nqueries,-1\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                parse(path)


if __name__ == "__main__":
    unittest.main()
