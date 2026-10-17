"""CPU-PROFILE-001C strict OFF/ON ABBA analyzer regression tests."""
import contextlib
import csv
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import analyze_cpu_profile_overhead as analyzer  # noqa: E402

CASES = ("mismatch32", "mismatch64", "timeout32_zero", "timeout64_zero")
HEADER = ["case", "iterations", "rounds", "min_ns", "median_ns", "max_ns"]


def write_csv(path, medians, *, corrupt=None):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(HEADER)
        for case in CASES:
            middle = medians[case]
            row = [case, 10000, 9, middle * 0.95, middle, middle * 1.05]
            if corrupt is not None and corrupt[0] == case:
                row[corrupt[1]] = corrupt[2]
            writer.writerow(row)


class ProfileOverheadAnalyzerTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.files = [Path(temp.name) / f"sample_{i}.csv" for i in range(4)]
        for i, path in enumerate(self.files):
            multiplier = (1.0, 1.5, 1.5, 1.0)[i]
            write_csv(path, {case: multiplier * 100 for case in CASES})

    def test_correct_paired_on_over_off(self):
        rows = analyzer.analyze(self.files)
        self.assertEqual(len(rows), 4)
        self.assertEqual(rows[0][0], "mismatch32")
        self.assertAlmostEqual(rows[0][3], 50.0)
        self.assertFalse(any(x[-1] for x in rows))

    def test_drift_yields_noisy_not_accepted(self):
        write_csv(self.files[-1], {case: 160.0 for case in CASES})
        self.assertTrue(all(row[-1] for row in analyzer.analyze(self.files)))

    def test_within_run_noise_flagged(self):
        write_csv(self.files[2], {case: 150.0 for case in CASES},
                  corrupt=("mismatch64", 5, 240.0))
        rows = analyzer.analyze(self.files)
        self.assertTrue(rows[1][-1])
        self.assertEqual(sum(row[-1] for row in rows), 1)

    def test_inconsistent_iteration_rejected(self):
        write_csv(self.files[0], {case: 100.0 for case in CASES},
                  corrupt=("mismatch32", 1, 500))
        with self.assertRaisesRegex(ValueError, "inconsistent iteration"):
            analyzer.analyze(self.files)

    def test_invalid_and_nonfinite_inputs_rejected(self):
        for invalid in ("nan", "-1"):
            write_csv(self.files[1], {case: 150.0 for case in CASES},
                      corrupt=("timeout32_zero", 4, invalid))
            with self.assertRaises(ValueError):
                analyzer.analyze(self.files)

    def test_explicit_synthetic_marker(self):
        old_argv = sys.argv
        try:
            sys.argv = ["analyzer", *(str(p) for p in self.files),
                        "--commit-sha", "x", "--platform", "linux"]
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                analyzer.main()
            self.assertIn("CPU_PROFILE_001C_ANALYZER_PASS", out.getvalue())
            self.assertIn("platform,linux", out.getvalue())
        finally:
            sys.argv = old_argv


if __name__ == "__main__":
    unittest.main()
