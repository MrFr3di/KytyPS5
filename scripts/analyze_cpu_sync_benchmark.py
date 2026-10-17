#!/usr/bin/env python3
"""CPU-SYNC-001: sanity-check and summarize *synthetic* paired Windows measurements.

No performance threshold is enforced on heterogeneous/noisy hosted VMs. Every
file comes from a different invocation of the actual built production-path
benchmark, with baseline/candidate order A-B-B-A on the same runner.
"""
import argparse
import csv
import math
import statistics
from pathlib import Path

REQUIRED = ("case", "iterations", "rounds", "min_ns", "median_ns", "max_ns")
CASES = ("mismatch32", "mismatch64", "timeout32_zero", "timeout64_zero")


def load(path):
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != list(REQUIRED):
            raise ValueError(f"{path}: unexpected CSV header {reader.fieldnames}")
        rows = list(reader)
    if len(rows) != len(CASES) or {row["case"] for row in rows} != set(CASES):
        raise ValueError(f"{path}: missing/duplicate/unknown benchmark cases")
    parsed = {}
    for row in rows:
        name = row["case"]
        iterations, rounds = int(row["iterations"]), int(row["rounds"])
        lo, median, hi = map(float, (row["min_ns"], row["median_ns"], row["max_ns"]))
        if iterations <= 0 or rounds < 9:
            raise ValueError(f"{path}: insufficient iterations or samples")
        if any(not math.isfinite(x) for x in (lo, median, hi)):
            raise ValueError(f"{path}: non-finite measurements")
        if not 0 < lo <= median <= hi:
            raise ValueError(f"{path}: invalid distribution")
        parsed[name] = (iterations, median, lo, hi)
    return parsed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline_first")
    parser.add_argument("candidate_first")
    parser.add_argument("candidate_second")
    parser.add_argument("baseline_second")
    parser.add_argument("--baseline-sha", required=True)
    parser.add_argument("--candidate-sha", required=True)
    args = parser.parse_args()
    samples = [load(p) for p in (
        args.baseline_first, args.candidate_first,
        args.candidate_second, args.baseline_second)]
    print("case,baseline_ns,candidate_ns,baseline_over_candidate,"
          "baseline_spread_pct,candidate_spread_pct,verdict")
    for case in CASES:
        rows = [file[case] for file in samples]
        if len({r[0] for r in rows}) != 1:
            raise ValueError(f"{case}: inconsistent iteration counts")
        base = statistics.median((rows[0][1], rows[3][1]))
        candidate = statistics.median((rows[1][1], rows[2][1]))
        base_spread = 100 * abs(rows[0][1] - rows[3][1]) / base
        cand_spread = 100 * abs(rows[1][1] - rows[2][1]) / candidate
        # Baseline must actually be an isolated source version; when the
        # branch is pristine, equal SHAs are expected and still useful.
        noisy = max(base_spread, cand_spread) > 20
        print(f"{case},{base:.3f},{candidate:.3f},{base/candidate:.3f},"
              f"{base_spread:.1f},{cand_spread:.1f},"
              f"{'NOISY_SYNTHETIC' if noisy else 'SYNTHETIC_ONLY'}")
    print(f"baseline_sha,{args.baseline_sha}")
    print(f"candidate_sha,{args.candidate_sha}")
    print("CPU_SYNC_001_ANALYZER_PASS")


if __name__ == "__main__":
    main()
