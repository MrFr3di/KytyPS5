#!/usr/bin/env python3
"""S4-B5-F: strict aggregate TextureCache canonical-shape CSV analysis.

Only measured calls reaching FindImagesInRegion are counted. No game/FPS
representativeness can be inferred from a successful CI synthetic fixture.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
try:
    from scripts.analyze_texture_range_telemetry import parse as parse_legacy, analyze as analyze_legacy
except ModuleNotFoundError:
    from analyze_texture_range_telemetry import parse as parse_legacy, analyze as analyze_legacy

SHAPES=("shape_queries","shape_l1_cross_queries","shape_pages_64plus",
        "shape_pages_256plus","shape_pages_1024plus",
        "shape_pages_256plus_same_l1","shape_pages_256plus_cross_l1")
UINT64_MAX=(1<<64)-1

def parse(path: Path) -> dict[str,int]:
    values=parse_legacy(path)
    missing=set(SHAPES)-values.keys()
    if missing:raise ValueError("missing shape fields: "+", ".join(sorted(missing)))
    return values

def analyze(values: dict[str,int], fixture: bool=False) -> dict:
    # Includes existing 7-bin telemetry accounting and generation invariants.
    previous=analyze_legacy(values)
    for key in SHAPES:
        value=values.get(key)
        if not isinstance(value,int) or isinstance(value,bool) or not 0<=value<=UINT64_MAX:
            raise ValueError("invalid unsigned shape counter: "+key)
    n=values["shape_queries"]
    cross=values["shape_l1_cross_queries"]
    p64=values["shape_pages_64plus"]
    p256=values["shape_pages_256plus"]
    p1024=values["shape_pages_1024plus"]
    same=values["shape_pages_256plus_same_l1"]
    across=values["shape_pages_256plus_cross_l1"]
    # Existing histogram gives an independent lower bound for total
    # queried pages. This prevents plausible-looking fabricated distributions.
    old_bins=("1","2_3","4_7","8_15","16_63","64_255","256_plus")
    minimums=(1,2,4,8,16,64,256)
    minimum_pages=sum(values[f"query_pages_{key}"]*minimum
                      for key,minimum in zip(old_bins,minimums))
    if values["queried_pages"]<minimum_pages:
        raise ValueError("query page total below histogram lower bound")
    if values["query_pages_256_plus"]==0:
        maximums=(1,3,7,15,63,255,0)
        upper=sum(values[f"query_pages_{key}"]*limit
                  for key,limit in zip(old_bins,maximums))
        if values["queried_pages"]>upper:
            raise ValueError("query page total above finite histogram upper bound")
    if n!=values["queries"]:
        raise ValueError("not exactly one valid shape per timed region query")
    if cross>n:raise ValueError("L1 crossing count exceeds queries")
    if p1024>p256 or p256>p64 or p64>n:
        raise ValueError("nonmonotonic query page thresholds")
    if p64!=values["query_pages_64_255"]+values["query_pages_256_plus"]:
        raise ValueError("64+ threshold not equal to old page bins")
    if p256!=values["query_pages_256_plus"]:
        raise ValueError("256+ threshold not equal to old page bin")
    if same+across!=p256:
        raise ValueError("256+ queries must partition exactly by L1 crossing")
    if across>cross or same>n-cross:
        raise ValueError("L1 partitions cannot exceed global crossing totals")
    def share(value):
        return round(value/n,6) if n else None
    return {
        "source_classification":"SYNTHETIC_CI_FIXTURE" if fixture else "UNVERIFIED_SOURCE",
        "observed_valid_region_lookups":n,
        "observed_invalid_region_lookups":values["invalid_queries"],
        "short_below_256_queries":n-p256,
        "short_below_256_share":share(n-p256),
        "page64plus_queries":p64,
        "page64plus_share":share(p64),
        "page256plus_queries":p256,
        "page256plus_share":share(p256),
        "page1024plus_queries":p1024,
        "page1024plus_share":share(p1024),
        "l1_cross_queries":cross,
        "l1_cross_share":share(cross),
        "page256plus_same_l1_queries":same,
        "page256plus_cross_l1_queries":across,
        "page256plus_same_l1_share":share(same),
        "mean_region_lookup_ns_with_telemetry":previous["mean_query_ns"],
        "production_go":"BLOCKED_NO_REPRESENTATIVE_GAME_TRACE",
        "limitations":[
            "Only valid calls reaching TextureCache::FindImagesInRegion are counted; descriptor cache hits and all frames are not.",
            "No game executable, playthrough, frame-time distribution, FPS or GPU timing provenance is established by CI fixture.",
            "Opt-in telemetry has measurement overhead; reported ns do not measure uninstrumented game performance.",
            "No exact quantiles, per-call records, virtual addresses or image IDs are stored."
        ]
    }

def main() -> int:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("csv_files",nargs="+",type=Path)
    p.add_argument("--fixture",action="store_true",
                   help="explicitly label all inputs as SYNTHETIC_CI_FIXTURE")
    args=p.parse_args()
    try:
        results=[{"file":str(path), **analyze(parse(path),args.fixture)}
                 for path in args.csv_files]
    except (OSError,ValueError) as exc:
        p.error(str(exc))
    print(json.dumps(results,indent=2,ensure_ascii=False))
    return 0
if __name__=="__main__":
    raise SystemExit(main())
