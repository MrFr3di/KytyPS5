#!/usr/bin/env python3
"""KYTY-INDEX-001: interpret aggregate, address-free TextureCache telemetry.

This script NEVER decides to enable H1 in production. A CSV from an
instrumented real workload reports only calls reaching the page table; it
does not reveal requests satisfied by descriptor caching before the lookup.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

BINS = ("1", "2_3", "4_7", "8_15", "16_63", "64_255", "256_plus")
GEN_BINS = ("0", "1", "2_3", "4_7", "8_15", "16_31", "32_plus")
REQUIRED = (
    "queries", "invalid_queries", "page_overlap_queries", "owners_inspected",
    "results", "queried_pages", "query_ns", "registrations", "unregistrations",
    "registered_pages", "unregistered_pages", "active_images_at_dump",
    "max_active_images", "completed_generations", "generations_with_queries",
    "generations_with_eight_large_queries",
    "max_large_queries_per_completed_generation",
    "completed_generation_large_queries",
    "completed_generation_large_queries_after_eight",
    "current_generation_queries", "current_generation_large_queries",
) + tuple(f"generation_large_queries_{b}" for b in GEN_BINS) + tuple(
    f"query_pages_{b}" for b in BINS
) + tuple(
    f"query_ns_pages_{b}" for b in BINS
)


def parse(path: Path) -> dict[str, int]:
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["metric", "value"]:
            raise ValueError("unexpected telemetry CSV header")
        result: dict[str, int] = {}
        for row in reader:
            key = row["metric"]
            if not key or key in result:
                raise ValueError(f"missing or duplicate metric: {key!r}")
            try:
                value = int(row["value"], 10)
            except (ValueError, TypeError) as exc:
                raise ValueError(f"not an integer: {key!r}") from exc
            if value < 0 or value > 2**64 - 1:
                raise ValueError(f"out of uint64 range: {key!r}")
            result[key] = value
    missing = set(REQUIRED) - set(result)
    if missing:
        raise ValueError(f"missing fields: {', '.join(sorted(missing))}")
    return result


def analyze(values: dict[str, int]) -> dict:
    q = values["queries"]
    b = [values[f"query_pages_{name}"] for name in BINS]
    ns = [values[f"query_ns_pages_{name}"] for name in BINS]
    generation_bins = [values[f"generation_large_queries_{name}"] for name in GEN_BINS]
    if sum(b) != q:
        raise ValueError("query page histogram does not sum to queries")
    if sum(ns) != values["query_ns"]:
        raise ValueError("query duration histograms do not sum to query_ns")
    if sum(generation_bins) != values["completed_generations"]:
        raise ValueError("generation histogram does not sum to completed generations")
    if (values["completed_generation_large_queries"] +
            values["current_generation_large_queries"] != sum(b[3:])):
        raise ValueError("completed plus open generation large calls must match histogram")
    if (values["completed_generation_large_queries_after_eight"] >
            values["completed_generation_large_queries"]):
        raise ValueError("post-threshold queries exceed completed-generation large calls")
    if values["queried_pages"] < q:
        raise ValueError("queried pages fewer than valid queries")
    if values["results"] > values["owners_inspected"]:
        raise ValueError("more unique results than inspected owners")
    if values["page_overlap_queries"] > q:
        raise ValueError("more page-overlap queries than total queries")
    if values["current_generation_large_queries"] > values["current_generation_queries"]:
        raise ValueError("open-generation large queries exceed all queries")
    if (values["generations_with_eight_large_queries"] >
            values["generations_with_queries"] or
            values["generations_with_queries"] > values["completed_generations"]):
        raise ValueError("inconsistent completed generation counters")
    if values["active_images_at_dump"] > values["max_active_images"]:
        raise ValueError("live registered count exceeds historical maximum")

    large_queries = sum(b[3:])
    large_time_ns = sum(ns[3:])
    g = values["generations_with_queries"]
    return {
        "index_queries": q,
        "invalid_range_queries": values["invalid_queries"],
        "large_8plus_pages_queries": large_queries,
        "large_query_share": round(large_queries / q, 6) if q else None,
        "mean_query_ns": round(values["query_ns"] / q, 2) if q else None,
        "mean_large_query_ns": round(large_time_ns / large_queries, 2)
        if large_queries else None,
        "mean_inspected_owners_per_query": round(values["owners_inspected"] / q, 3)
        if q else None,
        "registrations": values["registrations"],
        "unregistrations": values["unregistrations"],
        "completed_generations_with_queries": g,
        "completed_generations_with_8plus_large_queries":
            values["generations_with_eight_large_queries"],
        "qualifying_generation_share": round(
            values["generations_with_eight_large_queries"] / g, 6
        ) if g else None,
        "max_large_queries_in_completed_generation":
            values["max_large_queries_per_completed_generation"],
        "completed_generation_large_queries_after_eight":
            values["completed_generation_large_queries_after_eight"],
        "generation_large_query_histogram": dict(zip(GEN_BINS, generation_bins)),
        "open_generation_queries": values["current_generation_queries"],
        "open_generation_large_queries": values["current_generation_large_queries"],
        "decision": "INSUFFICIENT_FOR_PRODUCTION_GO",
        "limitations": [
            "No descriptor-cache hits or earlier warm requests are observed.",
            "No per-query p95 or exact per-generation count distribution.",
            "No H1 build cost or throughput measurement on real game workload.",
            "This diagnostic has nonzero clock/accounting overhead.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_files", nargs="+", type=Path,
                        help="CSV files from real opt-in Windows emulator runs")
    parser.add_argument("--json", action="store_true", help="write machine-readable JSON")
    args = parser.parse_args()
    all_results = []
    for path in args.csv_files:
        try:
            result = analyze(parse(path))
        except (OSError, ValueError) as exc:
            parser.error(f"{path}: {exc}")
        all_results.append({"file": str(path), **result})
    if args.json:
        print(json.dumps(all_results, indent=2, ensure_ascii=False))
    else:
        for result in all_results:
            print(f"{result['file']}: {result['index_queries']} index queries; "
                  f"{result['large_8plus_pages_queries']} cover >=8 pages; "
                  f"{result['completed_generations_with_8plus_large_queries']}/"
                  f"{result['completed_generations_with_queries']} query-active "
                  f"completed generations reach 8 large queries. "
                  "Decision: INSUFFICIENT_FOR_PRODUCTION_GO.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
