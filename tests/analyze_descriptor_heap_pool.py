#!/usr/bin/env python3
"""G3-D4: strict offline pool-epoch capacity audit, NOT a GPU benchmark.

Input: descriptor-heap-telemetry.csv from the runtime opt-in D4 producer.
The same parser is exercised by a SYNTHETIC C++ fixture in hosted CI.
"""
from __future__ import annotations

import csv
import pathlib
import re
import sys
from typing import Mapping

LIMIT = (1 << 64) - 1
BUCKETS = ("storage_buffer", "sampled_image", "storage_image", "sampler")
CAPACITY = (8192, 8192, 1024, 1024)
SOURCE_MAX_SETS = 1024

CORE = {
    "schema_version", "pool_creates", "pool_resets", "pool_retires",
    "peak_pending_pools", "cache_hits", "cache_misses", "allocation_attempts",
    "reject_out_of_pool", "reject_fragmented", "allocated_sets", "issued_sets",
    "discarded_unused_sets", "currently_cached_unused_sets",
    "peak_cached_unused_sets", "type_counts_known", "gpu_timing_known",
    "invariant_pass", "production_allocation_policy_modified",
}
EPOCH = {"pool_capacity_sets", "peak_pool_allocated_sets",
         "pool_budget_known", "pool_budget_feasible"}
for bucket in BUCKETS:
    CORE.update(f"{prefix}_{bucket}"
                for prefix in ("allocated", "issued", "discarded", "cached"))
    EPOCH.update((f"pool_capacity_{bucket}", f"peak_pool_allocated_{bucket}"))


def parse_csv(text: str) -> dict[str, int]:
    rows = list(csv.reader(text.splitlines()))
    if not rows or rows[0] != ["metric", "value"]:
        raise ValueError("expected two-column metric,value header")
    stats: dict[str, int] = {}
    for row in rows[1:]:
        if len(row) != 2:
            raise ValueError("unexpected CSV column count")
        key, raw = row
        if not re.fullmatch(r"[a-z][a-z0-9_]*", key):
            raise ValueError("invalid metric name")
        if key in stats:
            raise ValueError(f"duplicate metric {key}")
        if not raw.isascii() or not raw.isdecimal() or raw.startswith("+"):
            raise ValueError(f"nonnegative decimal required: {key}")
        value = int(raw)
        if value > LIMIT:
            raise ValueError(f"uint64 overflow: {key}")
        stats[key] = value
    if not CORE.issubset(stats):
        raise ValueError(f"missing core metrics: {sorted(CORE - stats)}")
    if stats["schema_version"] not in (1, 2):
        raise ValueError("unsupported schema_version")
    if stats["schema_version"] == 2 and not EPOCH.issubset(stats):
        raise ValueError(f"missing G3-D4 per-epoch metrics: {sorted(EPOCH - stats)}")
    return stats


def classify(m: Mapping[str, int]) -> str:
    # A v1 lifetime-only trace cannot prove or disprove per-pool capacity.
    if m["schema_version"] == 1:
        return "INSUFFICIENT_TRACE"
    if any(m[k] not in (0, 1) for k in ("invariant_pass", "type_counts_known",
           "pool_budget_known", "pool_budget_feasible", "gpu_timing_known",
           "production_allocation_policy_modified")):
        return "COUNTEREXAMPLE"
    if m["gpu_timing_known"] != 0 or m["production_allocation_policy_modified"] != 0:
        return "COUNTEREXAMPLE"
    if m["pool_creates"] < 1 or m["pool_resets"] > m["pool_retires"]:
        return "COUNTEREXAMPLE"
    if m["allocation_attempts"] < (m["reject_out_of_pool"] + m["reject_fragmented"]):
        return "COUNTEREXAMPLE"
    if m["issued_sets"] > m["allocated_sets"]:
        return "COUNTEREXAMPLE"
    remaining = m["allocated_sets"] - m["issued_sets"]
    if m["discarded_unused_sets"] > remaining:
        return "COUNTEREXAMPLE"
    if remaining - m["discarded_unused_sets"] != m["currently_cached_unused_sets"]:
        return "COUNTEREXAMPLE"
    if m["peak_pool_allocated_sets"] > m["allocated_sets"]:
        return "COUNTEREXAMPLE"
    if m["peak_cached_unused_sets"] > m["allocated_sets"]:
        return "COUNTEREXAMPLE"
    if m["pool_capacity_sets"] != SOURCE_MAX_SETS:
        return "COUNTEREXAMPLE"
    if any(m[f"pool_capacity_{k}"] != capacity
           for k, capacity in zip(BUCKETS, CAPACITY)):
        return "COUNTEREXAMPLE"
    if m["invariant_pass"] != 1:
        return "COUNTEREXAMPLE"
    # Sets alone can establish a contradiction; typed weights might be unknown.
    if m["peak_pool_allocated_sets"] > SOURCE_MAX_SETS:
        return "COUNTEREXAMPLE"
    if m["pool_budget_known"] != m["type_counts_known"]:
        return "COUNTEREXAMPLE"
    if not m["type_counts_known"]:
        if m["pool_budget_feasible"] != 0:
            return "COUNTEREXAMPLE"
        if any(m[f"{prefix}_{k}"] != 0 for k in BUCKETS
               for prefix in ("allocated", "issued", "discarded", "cached",
                              "peak_pool_allocated")):
            return "COUNTEREXAMPLE"
        return "INSUFFICIENT_TRACE"

    if m["allocated_sets"] == 0:
        return "COUNTEREXAMPLE"  # type_counts_known requires a typed allocation
    for k in BUCKETS:
        a, i, d, c = (m[f"{prefix}_{k}"]
                      for prefix in ("allocated", "issued", "discarded", "cached"))
        if i > a or d > a - i or c != a - i - d:
            return "COUNTEREXAMPLE"
        if m[f"peak_pool_allocated_{k}"] > a:
            return "COUNTEREXAMPLE"
    if m["pool_budget_feasible"] != 1:
        return "COUNTEREXAMPLE"
    if any(m[f"peak_pool_allocated_{k}"] > capacity
           for k, capacity in zip(BUCKETS, CAPACITY)):
        return "COUNTEREXAMPLE"
    # A capacity-consistent trace is NOT GPU time, FPS, or proof of optimality.
    return "CONSISTENT_DIAGNOSTIC"


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: analyze_descriptor_heap_pool.py descriptor-heap-telemetry.csv",
              file=sys.stderr)
        return 2
    try:
        raw = pathlib.Path(argv[1]).read_text(encoding="utf-8-sig")
        status = classify(parse_csv(raw))
    except (OSError, UnicodeError, ValueError) as error:
        print(f"INVALID_CSV: {error}", file=sys.stderr)
        return 2
    print(f"decision,{status},gpu_workload,UNVERIFIED,driver_time,UNKNOWN")
    return 0 if status == "CONSISTENT_DIAGNOSTIC" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
