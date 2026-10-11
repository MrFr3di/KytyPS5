#!/usr/bin/env python3
"""Validate bounded opt-in TextureCache workload attribution (KYTY-INDEX-001 G0).

Counters record stage elapsed wall time, NOT CPU cycles, GPU time, frame-time,
or an exhaustive emulator call graph. Stage self times exclude *instrumented*
nested stages on the same thread. A CSV alone never yields a production GO.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

STAGES = (
    "region_lookup", "image_discovery", "resolve_overlap", "image_refresh",
    "image_initialize", "image_copy", "cpu_alias_invalidate",
    "gpu_memory_invalidate", "garbage_collect", "register_image",
    "unregister_image", "resolve_texture", "rebind_images",
    "descriptor_commit", "texture_readback", "process_readbacks",
    "buffer_download", "buffer_upload", "buffer_synchronize",
    "buffer_acquire", "buffer_garbage_collect",
)
EVENTS = (
    "rebind_reuse", "rebind_missing", "rebind_unregistered",
    "rebind_needed", "descriptor_push_writes", "descriptor_set_writes",
    "buffer_upload_staged_bytes", "buffer_upload_regions",
    "buffer_upload_batches", "buffer_readback_packed_bytes",
    "buffer_readback_payload_bytes", "buffer_readback_regions",
    "texture_readback_requested_bytes", "image_upload_requests",
    "image_upload_guest_bytes",
)
MAX_U64 = (1 << 64) - 1


def parse(path: Path) -> dict[str, dict[str, dict[str, int]]]:
    rows: dict[str, dict[str, dict[str, int]]] = {"stage": {}, "event": {}}
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != ["kind", "name", "count", "inclusive_ns", "self_ns"]:
            raise ValueError("unexpected CSV header")
        for row in reader:
            if None in row or len(row) != 5:
                raise ValueError("invalid row shape")
            kind, name = row["kind"], row["name"]
            valid = STAGES if kind == "stage" else EVENTS if kind == "event" else ()
            if name not in valid or name in rows.get(kind, {}):
                raise ValueError(f"unknown/duplicate metric: {kind}/{name}")
            values = {}
            for key in ("count", "inclusive_ns", "self_ns"):
                raw = row[key]
                if raw is None or not raw or not raw.isascii() or not raw.isdecimal():
                    raise ValueError(f"invalid {key} for {name}")
                value = int(raw, 10)
                if value < 0 or value > MAX_U64:
                    raise ValueError(f"out of range {key} for {name}")
                values[key] = value
            if kind == "stage":
                if values["self_ns"] > values["inclusive_ns"]:
                    raise ValueError(f"self exceeds inclusive: {name}")
                if values["count"] == 0 and (values["inclusive_ns"] or values["self_ns"]):
                    raise ValueError(f"timing without calls: {name}")
            elif values["inclusive_ns"] or values["self_ns"]:
                raise ValueError(f"event contains timing: {name}")
            rows[kind][name] = values
    if set(rows["stage"]) != set(STAGES) or set(rows["event"]) != set(EVENTS):
        raise ValueError("incomplete metric schema")
    events = rows["event"]
    if events["buffer_upload_batches"]["count"] > events["buffer_upload_regions"]["count"]:
        raise ValueError("more upload batches than regions")
    staged_bytes = events["buffer_upload_staged_bytes"]["count"]
    if events["buffer_upload_regions"]["count"] == 0 and staged_bytes:
        raise ValueError("upload bytes without upload regions")
    # RegionManager returns aligned 4-KiB tracker-page ranges.
    if staged_bytes % 4096:
        raise ValueError("upload staged bytes not aligned to tracker pages")
    if events["buffer_readback_packed_bytes"]["count"] < events["buffer_readback_payload_bytes"]["count"]:
        raise ValueError("packed readback smaller than payload")
    if events["buffer_readback_regions"]["count"] == 0 and (
        events["buffer_readback_packed_bytes"]["count"] or
        events["buffer_readback_payload_bytes"]["count"]
    ):
        raise ValueError("readback bytes without regions")
    if events["image_upload_requests"]["count"] == 0 and events["image_upload_guest_bytes"]["count"]:
        raise ValueError("image upload bytes without requests")
    return rows


def analyze(rows: dict[str, dict[str, dict[str, int]]]) -> dict:
    stages = rows["stage"]
    total_self = sum(row["self_ns"] for row in stages.values())
    # A valid empty trace must not rank an unobserved stage as the bottleneck.
    ordered = sorted(
        ((name, metric) for name, metric in stages.items() if metric["count"] > 0),
        key=lambda item: item[1]["self_ns"], reverse=True
    )
    rebinding = sum(rows["event"][name]["count"] for name in
                    ("rebind_reuse", "rebind_missing",
                     "rebind_unregistered", "rebind_needed"))
    events = {name: metrics["count"] for name, metrics in rows["event"].items()}
    unpacked = events["buffer_readback_payload_bytes"]
    packed = events["buffer_readback_packed_bytes"]
    upload_regions = events["buffer_upload_regions"]
    return {
        "recorded_instrumented_self_wall_ns": total_self,
        "region_lookup_share_of_instrumented_self": (
            round(stages["region_lookup"]["self_ns"] / total_self, 6)
            if total_self else None
        ),
        "top_stages": [
            {"name": name, "calls": metric["count"],
             "inclusive_ns": metric["inclusive_ns"],
             "self_ns": metric["self_ns"]}
            for name, metric in ordered[:10]
        ],
        "rebind_reason_observations": rebinding,
        "rebind_reuse_share": (
            round(rows["event"]["rebind_reuse"]["count"] / rebinding, 6)
            if rebinding else None
        ),
        "transfer_volume_counters": {
            name: events[name] for name in EVENTS if name.endswith("_bytes")
        },
        "g1_transfer_granularity": {
            "buffer_upload_regions": upload_regions,
            "buffer_upload_batches": events["buffer_upload_batches"],
            "buffer_upload_tracker_page_instances": events["buffer_upload_staged_bytes"] // 4096,
            "buffer_upload_staged_bytes_per_region": (
                round(events["buffer_upload_staged_bytes"] / upload_regions, 2)
                if upload_regions else None
            ),
            "buffer_readback_regions": events["buffer_readback_regions"],
            "buffer_readback_payload_bytes": unpacked,
            "buffer_readback_packed_bytes": packed,
            "readback_packing_ratio": round(packed / unpacked, 6) if unpacked else None,
            "image_full_upload_requests": events["image_upload_requests"],
            "image_full_upload_guest_bytes": events["image_upload_guest_bytes"],
        },
        "decision": "INSUFFICIENT_FOR_PRODUCTION_GO",
        "limitations": [
            "Scope nanoseconds are elapsed wall time including waits, not CPU cycles or GPU timestamps.",
            "This is a non-exhaustive, nested call graph; self share is NOT percentage of total frame CPU time.",
            "No per-frame, FPS, GPU stall, shader compile, or global descriptor-cache hit rate is measured.",
            "Transfer byte counters describe requested/staged/packed amounts, not measured PCIe throughput.",
            "Upload region counts are MemoryTracker-selected copies, not a count of unique dirty pages.",
            "Full image upload guest bytes are requested footprints, not actual transferred GPU bytes.",
            "Instrumentation itself changes timing; compare identical opt-in builds and complete game traces.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_files", nargs="+", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    for path in args.csv_files:
        try:
            report = analyze(parse(path))
        except (ValueError, OSError) as exc:
            parser.error(f"{path}: {exc}")
        if args.json:
            print(json.dumps({"file": str(path), **report}, indent=2, ensure_ascii=False))
        else:
            print(f"{path}: top stage {report['top_stages'][0]['name'] if report['top_stages'] else 'none'}; "
                  f"index share of measured self={report['region_lookup_share_of_instrumented_self']}; "
                  "decision INSUFFICIENT_FOR_PRODUCTION_GO")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
