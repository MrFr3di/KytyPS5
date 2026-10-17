#!/usr/bin/env python3
"""CPU-PROFILE-001C: paired OFF/ON/ON/OFF overhead evidence (synthetic only).

Reuses the strict four-case CSV parser from CPU-SYNC-001. A successful parser
marker is NOT a performance acceptance verdict or proof of zero overhead.
"""
import argparse
import statistics
from analyze_cpu_sync_benchmark import CASES, load


def analyze(paths):
    samples = [load(p) for p in paths]
    rows = []
    for case in CASES:
        data = [sample[case] for sample in samples]
        if len({x[0] for x in data}) != 1:
            raise ValueError(f"{case}: inconsistent iteration counts")
        off = statistics.median((data[0][1], data[3][1]))
        on = statistics.median((data[1][1], data[2][1]))
        off_drift = 100.0 * abs(data[0][1] - data[3][1]) / off
        on_drift = 100.0 * abs(data[1][1] - data[2][1]) / on
        spreads = [100.0 * (x[3] - x[2]) / x[1] for x in data]
        off_spread = max(spreads[0], spreads[3])
        on_spread = max(spreads[1], spreads[2])
        noisy = max(off_drift, on_drift, off_spread, on_spread) > 20.0
        rows.append((case, off, on, 100.0 * (on / off - 1.0),
                     off_drift, on_drift, off_spread, on_spread, noisy))
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("off_first")
    parser.add_argument("on_first")
    parser.add_argument("on_second")
    parser.add_argument("off_second")
    parser.add_argument("--commit-sha", required=True)
    parser.add_argument("--platform", required=True)
    args = parser.parse_args()
    paths = (args.off_first, args.on_first, args.on_second, args.off_second)
    rows = analyze(paths)
    print("case,off_ns,on_ns,on_over_off_pct,off_between_run_pct,"
          "on_between_run_pct,off_within_run_pct,on_within_run_pct,verdict")
    for case, off, on, pct, off_drift, on_drift, off_within, on_within, noisy in rows:
        verdict = "NOISY_SYNTHETIC" if noisy else "SYNTHETIC_ONLY"
        print(f"{case},{off:.3f},{on:.3f},{pct:+.1f},"
              f"{off_drift:.1f},{on_drift:.1f},{off_within:.1f},"
              f"{on_within:.1f},{verdict}")
    print(f"commit_sha,{args.commit_sha}")
    print(f"platform,{args.platform}")
    print("CPU_PROFILE_001C_ANALYZER_PASS")


if __name__ == "__main__":
    main()
