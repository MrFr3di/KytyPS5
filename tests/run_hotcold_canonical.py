#!/usr/bin/env python3
"""KYTY-INDEX-001 S4-B5-E: native SlotId hot/cold walk matched CPU evidence."""
from __future__ import annotations
import argparse
import csv
from pathlib import Path
import statistics
import subprocess
import sys
NAMES=("dense_overlap","boundary_44bit","short_l1_crossing",
       "short_single_bucket","long_single_bucket","mid_single_bucket_64")
VARIANTS=("canonical_helper_legacy","inline_page_t256","hot_legacy_cold_skip_t256",
          "cold_full_dispatch_t256","manual_inline_t256","unconditional_bucket_skip")
BATCH=(8,512,2048,2048,128,128)
HEADER=("scenario","variant","repetition","owners","queries","batch","registration_ns",
        "query_ns","churn_ns","post_query_ns","native_checksum","canonical_buckets",
        "owner_visits","new_requests","adaptive_skip","adaptive_find")
STABLE=("owners","queries","batch","native_checksum","canonical_buckets","owner_visits",
        "adaptive_skip","adaptive_find")

def parse(text):
    lines=list(csv.reader(text.strip().splitlines()))
    if len(lines)!=1 or len(lines[0])!=len(HEADER):
        raise ValueError("wrong output shape")
    row=dict(zip(HEADER,lines[0]))
    if row["scenario"] not in NAMES or row["variant"] not in VARIANTS:
        raise ValueError("unknown scenario or mode")
    for k in HEADER[2:]:
        if not row[k].isascii() or not row[k].isdecimal():
            raise ValueError("expected unsigned numeric: "+k)
        row[k]=int(row[k])
    return row

def check_group(rows):
    if len(rows)!=len(VARIANTS) or {x["variant"] for x in rows}!=set(VARIANTS):
        raise ValueError("six distinct modes required")
    ref=rows[0]
    si=NAMES.index(ref["scenario"])
    for x in rows:
        if (x["scenario"],x["repetition"])!=(ref["scenario"],ref["repetition"]):
            raise ValueError("different native workload/repetition")
        for k in ("owners","queries","batch","native_checksum","canonical_buckets","owner_visits","new_requests"):
            if x[k]!=ref[k]:
                raise ValueError("native oracle/order/visit/allocator mismatch "+k)
        if x["queries"]!=192*BATCH[si] or x["batch"]!=BATCH[si]:
            raise ValueError("batch/query drift")
        if x["owners"]<8 or x["canonical_buckets"]<1 or x["owner_visits"]<1:
            raise ValueError("missing native coverage")
        if x["variant"] in VARIANTS[1:5]:
            if x["adaptive_skip"]+x["adaptive_find"]!=x["queries"]:
                raise ValueError("lost/double dispatch")
            if si in (1,2,3,5) and x["adaptive_skip"]:
                raise ValueError("short query must use the hot legacy arm")
            if si==4 and x["adaptive_skip"]!=x["queries"]:
                raise ValueError("long single bucket must use cold skip arm")
        elif x["adaptive_skip"] or x["adaptive_find"]:
            raise ValueError("baseline unexpectedly counted dispatch")
    for k in ("owners","queries","batch","native_checksum","canonical_buckets","owner_visits"):
        if len({x[k] for x in rows})!=1:
            raise ValueError("within-group parity missing: "+k)
    adaptive=[x for x in rows if x["variant"] in VARIANTS[1:5]]
    if len({(x["adaptive_skip"],x["adaptive_find"]) for x in adaptive})!=1:
        raise ValueError("hot/cold decision differs between equivalent policies")

def save(path,fields,rows):
    with Path(path).open("w",encoding="utf-8",newline="") as fp:
        writer=csv.DictWriter(fp,fieldnames=fields,lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

def main(args):
    p=argparse.ArgumentParser()
    p.add_argument("binary")
    p.add_argument("raw_csv")
    p.add_argument("summary_csv")
    opt=p.parse_args(args)
    records=[]
    for si,scenario in enumerate(NAMES):
        for rep in range(5):
            matched=[]
            for mode in (range(6) if rep%2==0 else range(5,-1,-1)):
                proc=subprocess.run([opt.binary,str(si),str(mode),str(rep)],
                    capture_output=True,text=True,check=False)
                if proc.returncode:
                    raise RuntimeError(f"native process {si}/{mode}/{rep}: {proc.stderr}")
                row=parse(proc.stdout)
                if (row["scenario"],row["variant"],row["repetition"])!=(
                    scenario,VARIANTS[mode],rep):
                    raise ValueError("wrong process labels")
                matched.append(row)
            check_group(matched)
            records.extend(matched)
    for scenario in NAMES:
        if len({x["native_checksum"] for x in records if x["scenario"]==scenario})!=1:
            raise ValueError("generation/order checksum changed across repetitions")
    summary=[]
    for si,scenario in enumerate(NAMES):
        base={r["repetition"]:r for r in records if r["scenario"]==scenario and r["variant"]==VARIANTS[0]}
        for variant in VARIANTS[1:]:
            trial={r["repetition"]:r for r in records if r["scenario"]==scenario and r["variant"]==variant}
            for metric in ("registration_ns","query_ns","churn_ns","post_query_ns","new_requests"):
                a=[base[i][metric] for i in range(5)]
                b=[trial[i][metric] for i in range(5)]
                median_a=statistics.median(a)
                median_b=statistics.median(b)
                paired=[a[i]/b[i] for i in range(5) if b[i]>0]
                summary.append({
                  "scenario":scenario,"variant":variant,"metric":metric,
                  "legacy_median":int(median_a),"candidate_median":int(median_b),
                  "ratio_legacy_over_candidate":f"{median_a/median_b:.6f}" if median_b else "NA",
                  "min_paired_ratio":f"{min(paired):.6f}" if len(paired)==5 else "NA",
                  "legacy_ns_per_query":f"{median_a/(96*BATCH[si]):.4f}" if metric in ("query_ns","post_query_ns") else "NA",
                  "candidate_ns_per_query":f"{median_b/(96*BATCH[si]):.4f}" if metric in ("query_ns","post_query_ns") else "NA",
                  "selected_skip":int(statistics.median(x["adaptive_skip"] for x in trial.values())),
                  "selected_find":int(statistics.median(x["adaptive_find"] for x in trial.values())),
                  "evidence":"WARM_SYNTHETIC_CPU_NO_GPU_FPS"
                })
    save(opt.raw_csv,HEADER,records)
    save(opt.summary_csv,("scenario","variant","metric","legacy_median","candidate_median",
        "ratio_legacy_over_candidate","min_paired_ratio","legacy_ns_per_query",
        "candidate_ns_per_query","selected_skip","selected_find","evidence"),summary)
    print("S4_B5E_HOTCOLD_PASS,scenarios,6,matched_groups,30,processes,180,"
          "oracle,NATIVE_SLOT_ID_PAGE_MAJOR,evidence,WARM_SYNTHETIC_CPU_NO_GPU_FPS")
    return 0
if __name__=="__main__":
    try:sys.exit(main(sys.argv[1:]))
    except (RuntimeError,ValueError,OSError) as ex:
        print("S4_B5E_REJECT: "+str(ex),file=sys.stderr)
        sys.exit(1)
