"""Stdlib-only tests for scripts/analyze_texture_range_telemetry.py."""
import tempfile
import unittest
from pathlib import Path

from scripts.analyze_texture_range_telemetry import BINS, analyze, parse


def data():
    base = dict.fromkeys([
        "queries", "invalid_queries", "page_overlap_queries", "owners_inspected",
        "results", "queried_pages", "query_ns", "registrations", "unregistrations",
        "registered_pages", "unregistered_pages", "active_images_at_dump",
        "max_active_images", "completed_generations", "generations_with_queries",
        "generations_with_eight_large_queries",
        "max_large_queries_per_completed_generation",
        "current_generation_queries", "current_generation_large_queries",
    ], 0)
    base.update({f"query_pages_{b}": 0 for b in BINS})
    base.update({f"query_ns_pages_{b}": 0 for b in BINS})
    base.update({
        "queries": 5, "query_ns": 100, "owners_inspected": 20,
        "results": 4, "completed_generations": 3,
        "generations_with_queries": 2, "generations_with_eight_large_queries": 1,
        "max_large_queries_per_completed_generation": 8,
        "query_pages_1": 2, "query_pages_8_15": 3,
        "query_ns_pages_1": 20, "query_ns_pages_8_15": 80,
    })
    return base


class TelemetryAnalyzerTests(unittest.TestCase):
    def test_measured_index_share_is_not_descriptor_cache_ratio(self):
        summary = analyze(data())
        self.assertEqual(summary["index_queries"], 5)
        self.assertAlmostEqual(summary["large_query_share"], 0.6)
        self.assertEqual(summary["mean_large_query_ns"], round(80/3, 2))
        self.assertEqual(summary["qualifying_generation_share"], 0.5)
        self.assertEqual(summary["decision"], "INSUFFICIENT_FOR_PRODUCTION_GO")
        self.assertIn("descriptor-cache", summary["limitations"][0])

    def test_invalid_invariants_rejected(self):
        cases = [
            ("queries", 6),
            ("query_ns", 101),
            ("results", 21),
            ("page_overlap_queries", 6),
            ("generations_with_eight_large_queries", 3),
            ("current_generation_large_queries", 5),
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
