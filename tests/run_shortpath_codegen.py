#!/usr/bin/env python3
"""S4-B5-D matched Windows short-path/codegen CPU evidence. No FPS claims."""
from __future__ import annotations
import argparse
import csv
from pathlib import Path
import statistics
import subprocess
import sys

NAMES=("dense_overlap","boundary_44bit","short_l1_crossing",
       "short_single_bucket","long_single_bucket","mid_single_bucket_64")
VARIANTS=("direct_page_find","canonical_helper_legacy","page_only_t256",
          "manual_page_t256","page_only_t1024","always_bucket_skip")
BATCH=(8,512,2048,2048,128,128)
HEADER=("scenario","variant","repetition","owners","queries","batch",
        "registration_ns","query_ns","churn_ns","post_query_ns",
        "native_checksum","canonical_buckets","owner_visits","new_requests",
        "adaptive_skip","adaptive_find")
STABLE=("owners","queries","batch","native_checksum","canonical_buckets",
        "owner_visits","new_requests")

def parse(text):
    rows=list(csv.reader(text.strip().splitlines()))
    if len(rows)!=1 or len(rows[0])!=len(HEADER):
        raise ValueError("bad one-line CSV shape")
    row=dict(zip(HEADER,rows[0]))
    if row["scenario"] not in NAMES or row["variant"] not in VARIANTS:
        raise ValueError("unknown scenario/mode")
    for k in HEADER[2:]:
        if not row[k].isascii() or not row[k].isdecimal():
            raise ValueError("nonnegative numeric field: "+k)
        row[k]=int(row[k])
    return row

def check_group(rows):
    if len(rows)!=6 or {r["variant"] for r in rows}!=set(VARIANTS):
        raise ValueError("six matched variants required")
    ref=rows[0]
    si=NAMES.index(ref["scenario"])
    for r in rows:
        if (r["scenario"],r["repetition"])!=(ref["scenario"],ref["repetition"]):
            raise ValueError("unmatched fixture/rep")
        for field in STABLE:
            if r[field]!=ref[field]:
                raise ValueError("native checksum/visit/heap parity: "+field)
        if r["owners"]<8 or r["canonical_buckets"]<1 or r["owner_visits"]<1:
            raise ValueError("insufficient native coverage")
        if r["queries"]!=192*BATCH[si] or r["batch"]!=BATCH[si]:
            raise ValueError("frozen batch or query count drift")
        if r["variant"] in VARIANTS[2:5]:
            if r["adaptive_find"]+r["adaptive_skip"]!=r["queries"]:
                raise ValueError("adaptive dispatch lost queries")
            if si in (1,2,3,5) and r["adaptive_skip"]:
                raise ValueError("short/mid false branch unexpectedly skipped")
            if si==4:
                if r["variant"] in VARIANTS[2:4] and r["adaptive_skip"]!=r["queries"]:
                    raise ValueError("long page T256 must skip")
                if r["variant"]=="page_only_t1024" and r["adaptive_skip"]:
                    raise ValueError("long page T1024 must find")
        elif r["adaptive_find"] or r["adaptive_skip"]:
            raise ValueError("nonadaptive mode reported selection")

def save(path,fields,rows):
    with Path(path).open("w",encoding="utf-8",newline="") as out:
        writer=csv.DictWriter(out,fieldnames=fields,lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

def main(argv):
    p=argparse.ArgumentParser()
    p.add_argument("binary")
    p.add_argument("raw_csv")
    p.add_argument("summary_csv")
    a=p.parse_args(argv)
    raw=[]
    for si,scenario in enumerate(NAMES):
        for rep in range(5):
            group=[]
            order=range(6) if rep%2==0 else range(5,-1,-1)
            for mode in order:
                proc=subprocess.run([a.binary,str(si),str(mode),str(rep)],
                    capture_output=True,text=True,check=False)
                if proc.returncode:
                    raise RuntimeError(f"native oracle {si}/{mode}/{rep}: {proc.stderr}")
                row=parse(proc.stdout)
                if (row["scenario"],row["variant"],row["repetition"])!=(
                    scenario,VARIANTS[mode],rep):
                    raise ValueError("subprocess label mismatch")
                group.append(row)
            check_group(group)
            raw.extend(group)
    for scenario in NAMES:
        checks={r["native_checksum"] for r in raw if r["scenario"]==scenario}
        if len(checks)!=1:raise ValueError("cross-repetition ID checksum mismatch")
    for mode in VARIANTS[2:4]:
        rr=[r for r in raw if r["variant"]==mode]
        if not any(r["adaptive_skip"] for r in rr) or not any(r["adaptive_find"] for r in rr):
            raise ValueError("T256 both branches must be covered")
    if any(r["adaptive_skip"] for r in raw if r["variant"]=="page_only_t1024"):
        raise ValueError("T1024 false-path control inadvertently skipped")
    summary=[]
    for si,scenario in enumerate(NAMES):
        ref={r["repetition"]:r for r in raw if r["scenario"]==scenario and r["variant"]=="canonical_helper_legacy"}
        for mode in VARIANTS:
            if mode=="canonical_helper_legacy":continue
            candidate={r["repetition"]:r for r in raw if r["scenario"]==scenario and r["variant"]==mode}
            for metric in ("registration_ns","query_ns","churn_ns","post_query_ns","new_requests"):
                before=[ref[i][metric] for i in range(5)]
                after=[candidate[i][metric] for i in range(5)]
                m1=statistics.median(before);m2=statistics.median(after)
                ratios=[before[i]/after[i] for i in range(5) if after[i]>0]
                summary.append({
                    "scenario":scenario,"variant":mode,"metric":metric,
                    "legacy_median":int(m1),"candidate_median":int(m2),
                    "ratio_legacy_over_candidate":f"{m1/m2:.6f}" if m2 else "NA",
                    "worst_paired_ratio":f"{min(ratios):.6f}" if len(ratios)==5 else "NA",
                    "legacy_ns_per_lookup":f"{m1/(96*BATCH[si]):.4f}" if metric in ("query_ns","post_query_ns") else "NA",
                    "candidate_ns_per_lookup":f"{m2/(96*BATCH[si]):.4f}" if metric in ("query_ns","post_query_ns") else "NA",
                    "batch":BATCH[si],
                    "selected_skip_queries":int(statistics.median(row["adaptive_skip"] for row in candidate.values())),
                    "selected_find_queries":int(statistics.median(row["adaptive_find"] for row in candidate.values())),
                    "evidence":"WARM_SYNTHETIC_CPU_NO_FPS"})
    save(a.raw_csv,HEADER,raw)
    save(a.summary_csv,("scenario","variant","metric","legacy_median","candidate_median",
         "ratio_legacy_over_candidate","worst_paired_ratio","legacy_ns_per_lookup",
         "candidate_ns_per_lookup","batch","selected_skip_queries",
         "selected_find_queries","evidence"),summary)
    print("S4_B5D_SHORTPATH_PASS,scenarios,6,matched_groups,30,processes,180,"
          "oracle,NATIVE_SLOT_ID_PAGE_MAJOR,evidence,WARM_SYNTHETIC_CPU_NO_FPS")
    return 0

if __name__=="__main__":
    try:sys.exit(main(sys.argv[1:]))
    except (ValueError,RuntimeError,OSError) as ex:
        print("S4_B5D_REJECT: "+str(ex),file=sys.stderr)
        sys.exit(1)
