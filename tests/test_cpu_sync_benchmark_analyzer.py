"""Executable regression tests for the CPU-SYNC-001 ABBA benchmark analyzer.

No game data, external packages, or GPU are required.
"""
import contextlib
import csv
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "analyze_cpu_sync_benchmark.py"
SPEC = importlib.util.spec_from_file_location("cpu_sync_analyzer", SCRIPT)
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)

CASES = (
    "mismatch32", "mismatch64", "timeout32_zero", "timeout64_zero"
)
HEADER = list(analyzer.REQUIRED)


def write_csv(path, medians, *, corrupt=None):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(HEADER)
        for case in CASES:
            median = medians[case]
            row = [case, 10000, 9, median * 0.95, median, median * 1.05]
            if corrupt is not None and corrupt[0] == case:
                row[corrupt[1]] = corrupt[2]
            writer.writerow(row)


class CpuSyncAnalyzerTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.root = Path(self.dir.name)
        self.files = [self.root / f"round_{i}.csv" for i in range(4)]
        for n, path in enumerate(self.files):
            multiplier = (2.0, 1.0, 1.0, 2.0)[n]
            write_csv(path, {case: 100.0 * multiplier for case in CASES})

    def run_analysis(self):
        old_argv = sys.argv
        try:
            sys.argv = [
                str(SCRIPT), *(str(path) for path in self.files),
                "--baseline-sha", "baseline", "--candidate-sha", "candidate",
            ]
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                analyzer.main()
            return output.getvalue()
        finally:
            sys.argv = old_argv

    def test_paired_works_with_expected_speed_ratio(self):
        text = self.run_analysis()
        self.assertIn("mismatch32,200.000,100.000,2.000", text)
        self.assertIn("CPU_SYNC_001_ANALYZER_PASS", text)
        self.assertEqual(text.count("SYNTHETIC_ONLY"), 4)

    def test_detects_between_process_drift(self):
        write_csv(self.files[3], {case: 350.0 for case in CASES})
        self.assertEqual(self.run_analysis().count("NOISY_SYNTHETIC"), 4)

    def test_rejects_nonfinite_measurement(self):
        write_csv(self.files[1], {case: 100.0 for case in CASES},
                  corrupt=("mismatch32", 4, "nan"))
        with self.assertRaisesRegex(ValueError, "non-finite"):
            analyzer.load(self.files[1])

    def test_rejects_invalid_ordering(self):
        write_csv(self.files[1], {case: 100.0 for case in CASES},
                  corrupt=("mismatch64", 3, "110"))
        with self.assertRaisesRegex(ValueError, "invalid distribution"):
            analyzer.load(self.files[1])

    def test_rejects_short_round_count(self):
        write_csv(self.files[2], {case: 100.0 for case in CASES},
                  corrupt=("timeout32_zero", 2, "8"))
        with self.assertRaisesRegex(ValueError, "insufficient"):
            analyzer.load(self.files[2])

    def test_rejects_duplicate_case(self):
        rows = list(csv.reader(self.files[2].open(newline="", encoding="utf-8")))
        rows[-1][0] = "mismatch32"
        with self.files[2].open("w", newline="", encoding="utf-8") as stream:
            csv.writer(stream).writerows(rows)
        with self.assertRaisesRegex(ValueError, "missing/duplicate"):
            analyzer.load(self.files[2])

    def test_rejects_incompatible_sample_counts(self):
        write_csv(self.files[2], {case: 100.0 for case in CASES},
                  corrupt=("mismatch64", 1, "5000"))
        with self.assertRaisesRegex(ValueError, "inconsistent iteration"):
            self.run_analysis()


if __name__ == "__main__":
    unittest.main()
