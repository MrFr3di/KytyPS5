#!/usr/bin/env python3
"""S4-B5-A: deterministic native SlotId canonical bucket-walk CPU replay."""
from __future__ import annotations
import argparse
import csv
import pathlib
import statistics
import subprocess
import sys

NAMES=("dense_overlap","sparse_multibucket","churn_buckets","boundary_44bit")
VARIANTS=("canonical_page_find","canonical_l1_bucket_skip")
HEADER=("scenario","variant","repetition","owners","queries",
        "registration_ns","query_ns","churn_ns","post_query_ns",
        "native_checksum","canonical_buckets","owner_visits","new_requests")

def parse(text):
    rows=list(csv.reader(text.strip().splitlines()))
    if len(rows)!=1 or len(rows[0])!=len(HEADER):raise ValueError("bad CSV shape")
    d=dict(zip(HEADER,rows[0]))
    if d["scenario"] not in NAMES or d["variant"] not in VARIANTS:
        raise ValueError("wrong scenario/variant")
    for k in HEADER[2:]:
        v=d[k]
        if not v.isascii() or not v.isdecimal():
            raise ValueError(f"nonnegative integer required for {k}")
        d[k]=int(v)
    return d

def check_pair(a,b):
    if {a["variant"],b["variant"]}!=set(VARIANTS):
        raise ValueError("paired modes missing")
    if a["scenario"]!=b["scenario"] or a["repetition"]!=b["repetition"]:
        raise ValueError("paired trace differs")
    for k in ("owners","queries","native_checksum","canonical_buckets",
              "owner_visits","new_requests"):
        if a[k]!=b[k]:raise ValueError(f"canonical parity failed: {k}")
    if a["queries"]!=192 or a["owners"]<8 or a["canonical_buckets"]<1:
        raise ValueError("unexpected frozen trace")
    if not a["owner_visits"]:
        raise ValueError("empty owner coverage")

def rows_to_file(filename,fields,rows):
    with pathlib.Path(filename).open("w",encoding="utf-8",newline="") as out:
        writer=csv.DictWriter(out,fieldnames=fields,lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

def main(argv):
    p=argparse.ArgumentParser()
    p.add_argument("binary")
    p.add_argument("raw_csv")
    p.add_argument("summary_csv")
    args=p.parse_args(argv)
    output=[]
    for scenario in range(4):
        for repetition in range(3):
            group=[]
            for mode in ((0,1) if repetition%2==0 else (1,0)):
                run=subprocess.run([args.binary,str(scenario),str(mode),str(repetition)],
                        capture_output=True,text=True,check=False)
                if run.returncode:
                    raise RuntimeError(f"oracle/benchmark {scenario}/{mode}/{repetition}: {run.stderr}")
                row=parse(run.stdout)
                if (row["scenario"]!=NAMES[scenario] or
                    row["variant"]!=VARIANTS[mode] or row["repetition"]!=repetition):
                    raise ValueError("wrong process label")
                group.append(row)
            check_pair(*group)
            output.extend(group)
    for scenario in NAMES:
        if len({r["native_checksum"] for r in output if r["scenario"]==scenario})!=1:
            raise ValueError("non-deterministic native key replay")
    summary=[]
    for scenario in NAMES:
        old=[r for r in output if r["scenario"]==scenario and r["variant"]==VARIANTS[0]]
        new=[r for r in output if r["scenario"]==scenario and r["variant"]==VARIANTS[1]]
        for metric in ("registration_ns","query_ns","churn_ns","post_query_ns",
                       "new_requests"):
            a=int(statistics.median(r[metric] for r in old))
            b=int(statistics.median(r[metric] for r in new))
            summary.append({"scenario":scenario,"metric":metric,
               "page_find_median":a,"bucket_skip_median":b,
               "ratio_page_find_over_skip":f"{a/b:.6f}" if b else "NA",
               "evidence":"SYNTHETIC_CPU_ONLY"})
    rows_to_file(args.raw_csv,HEADER,output)
    rows_to_file(args.summary_csv,("scenario","metric","page_find_median",
                 "bucket_skip_median","ratio_page_find_over_skip","evidence"),summary)
    print("S4_B5A_CANONICAL_PASS,scenarios,4,pairs,12,processes,24,"
          "semantic_oracle,PAGE_MAJOR_NATIVE_SLOT_ID,production_default,OFF,"
          "source,SYNTHETIC_CPU_ONLY")
    return 0

if __name__=="__main__":
    try:sys.exit(main(sys.argv[1:]))
    except (ValueError,RuntimeError,OSError) as exc:
        print(f"S4_B5A_REJECT: {exc}",file=sys.stderr)
        sys.exit(1)
