#!/usr/bin/env python3
"""S4-B4b native SlotId A/B benchmark driver; synthetic CPU evidence ONLY."""
from __future__ import annotations
import argparse
import csv
import pathlib
import statistics
import subprocess
import sys

COLUMNS=("scenario","variant","repetition","owners","queries",
 "register_ns","warmup_ns","query_ns","churn_ns","rewarm_ns","requery_ns",
 "register_new_calls","warmup_new_calls","query_new_calls","churn_new_calls",
 "rewarm_new_calls","requery_new_calls",
 "live_after_register_bytes","live_after_warm_bytes","live_after_query_bytes",
 "live_after_churn_bytes","peak_live_bytes","requested_total_bytes",
 "new_calls","delete_calls","native_checksum","canonical_buckets",
 "sidecar_nominal_payload_bytes","snapshot_builds","sidecar_answered",
 "canonical_fallbacks","has_sidecar")
SCENARIOS=("dense_overlap","sparse_disjoint","generation_churn")
VARIANTS=("canonical_only","canonical_plus_native_lazy")
PHASES=("register_ns","warmup_ns","query_ns","churn_ns","rewarm_ns","requery_ns")
MUST_MATCH=("owners","queries","native_checksum","canonical_buckets")

def parse_line(raw):
    values=list(csv.reader(raw.strip().splitlines()))
    if len(values)!=1 or len(values[0])!=len(COLUMNS):
        raise ValueError("one row of exactly 32 CSV fields required")
    row=dict(zip(COLUMNS,values[0]))
    if row["scenario"] not in SCENARIOS or row["variant"] not in VARIANTS:
        raise ValueError("unknown scenario/variant")
    for key in COLUMNS[2:]:
        value=row[key]
        if not value.isascii() or not value.isdecimal():
            raise ValueError("invalid unsigned "+key)
        row[key]=int(value)
    return row

def verify_pair(a,b):
    if {a["variant"],b["variant"]}!=set(VARIANTS):
        raise ValueError("both benchmark variants required")
    if a["scenario"]!=b["scenario"] or a["repetition"]!=b["repetition"]:
        raise ValueError("unmatched pairing")
    for key in MUST_MATCH:
        if a[key]!=b[key]:raise ValueError("native-key parity failure: "+key)
    canonical=a if a["variant"]==VARIANTS[0] else b
    sidecar=b if canonical is a else a
    if (canonical["has_sidecar"]!=0 or sidecar["has_sidecar"]!=1 or
        canonical["snapshot_builds"]!=0 or canonical["sidecar_answered"]!=0 or
        canonical["canonical_fallbacks"]!=0 or
        canonical["sidecar_nominal_payload_bytes"]!=0):
        raise ValueError("canonical-only baseline wrongly uses sidecar")
    if not (sidecar["snapshot_builds"]>=2 and sidecar["sidecar_answered"]>0 and
            sidecar["canonical_fallbacks"]>0 and
            sidecar["sidecar_nominal_payload_bytes"]>0):
        raise ValueError("sidecar build/rebuild/fallback not exercised")
    if a["queries"]!=648 or a["owners"]==0 or a["canonical_buckets"]==0:
        raise ValueError("unexpected synthetic trace geometry")
    for row in (a,b):
        if (row["peak_live_bytes"]<row["live_after_register_bytes"] or
            row["peak_live_bytes"]<row["live_after_query_bytes"] or
            row["new_calls"]<row["delete_calls"] or
            row["requested_total_bytes"]<row["peak_live_bytes"]):
            raise ValueError("invalid tagged allocation ledger")
        phases=("register_new_calls","warmup_new_calls","query_new_calls",
                "churn_new_calls","rewarm_new_calls","requery_new_calls")
        if row["new_calls"]<sum(row[k] for k in phases):
            raise ValueError("new calls below sum of measured phases")

def summarize(rows):
    summary=[]
    for name in SCENARIOS:
        a=[r for r in rows if r["scenario"]==name and r["variant"]==VARIANTS[0]]
        b=[r for r in rows if r["scenario"]==name and r["variant"]==VARIANTS[1]]
        if len(a)!=3 or len(b)!=3:
            raise ValueError("three independent processes per variant required")
        for metric in (*PHASES,"peak_live_bytes","new_calls","requested_total_bytes"):
            x=int(statistics.median(r[metric] for r in a))
            y=int(statistics.median(r[metric] for r in b))
            summary.append(dict(scenario=name,metric=metric,
              canonical_median=x,sidecar_median=y,
              ratio_canonical_over_sidecar=f"{x/y:.6f}" if y else "NA",
              evidence="SYNTHETIC_CPU_ONLY"))
    return summary

def main(argv):
    p=argparse.ArgumentParser()
    p.add_argument("binary")
    p.add_argument("raw_csv")
    p.add_argument("summary_csv")
    a=p.parse_args(argv)
    rows=[]
    for scenario in range(3):
        for rep in range(3):
            pair=[]
            for variant in ((0,1) if rep%2==0 else (1,0)):
                run=subprocess.run([a.binary,str(scenario),str(variant),str(rep)],
                                   text=True,capture_output=True,check=False)
                if run.returncode!=0:
                    raise RuntimeError(f"fixture {scenario}/{variant}/{rep}: {run.stderr}")
                row=parse_line(run.stdout)
                if (row["scenario"]!=SCENARIOS[scenario] or
                    row["variant"]!=VARIANTS[variant] or
                    row["repetition"]!=rep):
                    raise ValueError("mislabeled fixture output")
                pair.append(row)
            verify_pair(*pair)
            rows.extend(pair)
    for scenario in SCENARIOS:
        if len({r["native_checksum"] for r in rows if r["scenario"]==scenario})!=1:
            raise ValueError("native SlotId semantic checksum not reproducible")
    outputs=[
       (a.raw_csv,COLUMNS,rows),
       (a.summary_csv,("scenario","metric","canonical_median",
                        "sidecar_median","ratio_canonical_over_sidecar","evidence"),
        summarize(rows))
    ]
    for path,keys,data in outputs:
        with pathlib.Path(path).open("w",newline="",encoding="utf-8") as stream:
            writer=csv.DictWriter(stream,fieldnames=keys,lineterminator="\n")
            writer.writeheader()
            writer.writerows(data)
    print("S4_B4B_MATCHED_PASS,scenarios,3,pairs,9,processes,18,"
          "reference,INDEPENDENT_NATIVE_ORACLE,source,SYNTHETIC_CPU_ONLY,"
          "production_changes,0")
    return 0

if __name__=="__main__":
    try: sys.exit(main(sys.argv[1:]))
    except (ValueError,RuntimeError,OSError) as e:
        print(f"S4_B4B_REJECT: {e}",file=sys.stderr)
        sys.exit(1)
