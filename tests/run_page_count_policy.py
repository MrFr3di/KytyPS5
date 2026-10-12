#!/usr/bin/env python3
"""S4-B5-C: frozen matched warm synthetic CPU comparison, real native keys."""
from __future__ import annotations
import argparse
import csv
from pathlib import Path
import statistics
import subprocess
import sys

NAMES=("dense_overlap","sparse_multibucket","churn_buckets","boundary_44bit",
       "short_l1_crossing","long_single_bucket","mid_single_bucket_64")
VARIANTS=("canonical_page_find","canonical_l1_bucket_skip","cross_l1_t256",
          "page_only_t64","page_only_t256","page_only_t1024")
BATCH=(8,4,8,128,256,128,128)
HEADER=("scenario","variant","repetition","owners","queries","batch",
        "registration_ns","query_ns","churn_ns","post_query_ns","native_checksum",
        "canonical_buckets","owner_visits","new_requests","adaptive_skip","adaptive_find")
STABLE=("owners","queries","batch","native_checksum","canonical_buckets",
        "owner_visits","new_requests")

def parse(text):
    rows=list(csv.reader(text.strip().splitlines()))
    if len(rows)!=1 or len(rows[0])!=len(HEADER):
        raise ValueError("bad CSV shape")
    d=dict(zip(HEADER,rows[0]))
    if d["scenario"] not in NAMES or d["variant"] not in VARIANTS:
        raise ValueError("bad mode/scenario")
    for k in HEADER[2:]:
        if not d[k].isascii() or not d[k].isdecimal():
            raise ValueError("unsigned decimal expected: "+k)
        d[k]=int(d[k])
    return d

def check_group(group):
    if len(group)!=len(VARIANTS) or {r["variant"] for r in group}!=set(VARIANTS):
        raise ValueError("missing or duplicated candidate")
    ref=group[0]
    if ref["scenario"] not in NAMES:
        raise ValueError("unknown scenario")
    scenario=NAMES.index(ref["scenario"])
    for r in group:
        if (r["scenario"],r["repetition"])!=(ref["scenario"],ref["repetition"]):
            raise ValueError("group identity mismatch")
        for k in STABLE:
            if r[k]!=ref[k]:
                raise ValueError("native page/ID/order/allocation parity: "+k)
        if r["batch"]!=BATCH[scenario] or r["queries"]!=192*BATCH[scenario]:
            raise ValueError("nonfrozen batch or query count")
        if r["owners"]<8 or r["canonical_buckets"]<1 or r["owner_visits"]<1:
            raise ValueError("missing native coverage")
        if r["variant"] in VARIANTS[2:]:
            if r["adaptive_skip"]+r["adaptive_find"]!=r["queries"]:
                raise ValueError("missing or double dispatch")
        elif r["adaptive_skip"] or r["adaptive_find"]:
            raise ValueError("baseline unexpectedly dispatched")
        if scenario==4 and r["adaptive_skip"]:
            raise ValueError("short L1 crossing must remain legacy")
        if scenario==5:
            if r["variant"]=="page_only_t256" and r["adaptive_skip"]!=r["queries"]:
                raise ValueError("long single-bucket must use page-only")
            if r["variant"] in ("cross_l1_t256","page_only_t1024") and r["adaptive_skip"]:
                raise ValueError("long single-bucket cross/1024 must use legacy")
        if scenario==6:
            if r["variant"]=="page_only_t64" and r["adaptive_skip"]!=r["queries"]:
                raise ValueError("mid single-bucket should use T64")
            if r["variant"] in ("cross_l1_t256","page_only_t256","page_only_t1024") and r["adaptive_skip"]:
                raise ValueError("mid single-bucket rejected threshold must use legacy")

def save(path,headers,rows):
    with Path(path).open("w",encoding="utf-8",newline="") as output:
        writer=csv.DictWriter(output,fieldnames=headers,lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

def main(args):
    p=argparse.ArgumentParser()
    p.add_argument("binary")
    p.add_argument("raw_csv")
    p.add_argument("summary_csv")
    opts=p.parse_args(args)
    records=[]
    for si,scenario in enumerate(NAMES):
        for rep in range(3):
            group=[]
            modes=range(6) if rep%2==0 else range(5,-1,-1)
            for mode in modes:
                process=subprocess.run([opts.binary,str(si),str(mode),str(rep)],
                    capture_output=True,text=True,check=False)
                if process.returncode:
                    raise RuntimeError(f"oracle {scenario}/{mode}/{rep}: {process.stderr}")
                row=parse(process.stdout)
                if (row["scenario"],row["variant"],row["repetition"])!=(
                    scenario,VARIANTS[mode],rep):
                    raise ValueError("subprocess mislabeled")
                group.append(row)
            check_group(group)
            records.extend(group)
    for scenario in NAMES:
        if len({x["native_checksum"] for x in records if x["scenario"]==scenario})!=1:
            raise ValueError("unstable checksum across repetitions")
    for v in VARIANTS[2:]:
        if not any(r["adaptive_skip"] for r in records if r["variant"]==v):
            raise ValueError("uncovered true dispatch: "+v)
        if not any(r["adaptive_find"] for r in records if r["variant"]==v):
            raise ValueError("uncovered false dispatch: "+v)
    summary=[]
    for si,scenario in enumerate(NAMES):
        legacy={x["repetition"]:x for x in records if x["scenario"]==scenario and x["variant"]==VARIANTS[0]}
        for variant in VARIANTS[1:]:
            candidate={x["repetition"]:x for x in records if x["scenario"]==scenario and x["variant"]==variant}
            for metric in ("registration_ns","query_ns","churn_ns","post_query_ns","new_requests"):
                a=[legacy[i][metric] for i in range(3)]
                b=[candidate[i][metric] for i in range(3)]
                before=statistics.median(a)
                after=statistics.median(b)
                ratios=[a[i]/b[i] for i in range(3) if b[i]>0]
                summary.append({
                    "scenario":scenario,"variant":variant,"metric":metric,
                    "legacy_median":int(before),"candidate_median":int(after),
                    "ratio_legacy_over_candidate":f"{before/after:.6f}" if after else "NA",
                    "worst_paired_ratio":f"{min(ratios):.6f}" if len(ratios)==3 else "NA",
                    "legacy_ns_per_lookup":f"{before/(96*BATCH[si]):.3f}" if metric in ("query_ns","post_query_ns") else "NA",
                    "candidate_ns_per_lookup":f"{after/(96*BATCH[si]):.3f}" if metric in ("query_ns","post_query_ns") else "NA",
                    "batch":BATCH[si],
                    "selected_skip_queries":int(statistics.median(x["adaptive_skip"] for x in candidate.values())),
                    "selected_find_queries":int(statistics.median(x["adaptive_find"] for x in candidate.values())),
                    "evidence":"WARM_REPEATED_SYNTHETIC_CPU_ONLY"})
    save(opts.raw_csv,HEADER,records)
    save(opts.summary_csv,("scenario","variant","metric","legacy_median",
      "candidate_median","ratio_legacy_over_candidate","worst_paired_ratio",
      "legacy_ns_per_lookup","candidate_ns_per_lookup","batch",
      "selected_skip_queries","selected_find_queries","evidence"),summary)
    print("S4_B5C_PAGE_COUNT_PASS,scenarios,7,matched_groups,21,processes,126,"
          "oracle,INDEPENDENT_PAGE_MAJOR_SLOT_ID,production_default,OFF,"
          "evidence,WARM_REPEATED_SYNTHETIC_CPU_ONLY")
    return 0

if __name__=="__main__":
    try:sys.exit(main(sys.argv[1:]))
    except (RuntimeError,ValueError,OSError) as e:
        print("S4_B5C_REJECT: "+str(e),file=sys.stderr)
        sys.exit(1)
