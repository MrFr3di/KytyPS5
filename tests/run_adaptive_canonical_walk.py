#!/usr/bin/env python3
"""S4-B5-B native-key matched threshold sweep; CPU only, no FPS inference."""
from __future__ import annotations
import argparse
import csv
from pathlib import Path
import statistics
import subprocess
import sys

NAMES=("dense_overlap","sparse_multibucket","churn_buckets",
       "boundary_44bit","short_l1_crossing","long_single_bucket")
VARIANTS=("canonical_page_find","canonical_l1_bucket_skip",
          "adaptive_t16","adaptive_t64","adaptive_t256","adaptive_t1024")
HEADER=("scenario","variant","repetition","owners","queries","registration_ns",
        "query_ns","churn_ns","post_query_ns","native_checksum",
        "canonical_buckets","owner_visits","new_requests",
        "adaptive_skip","adaptive_find")
STABLE=("owners","queries","native_checksum","canonical_buckets",
        "owner_visits","new_requests")

def parse(text):
    rows=list(csv.reader(text.strip().splitlines()))
    if len(rows)!=1 or len(rows[0])!=len(HEADER):
        raise ValueError("bad CSV shape")
    d=dict(zip(HEADER,rows[0]))
    if d["scenario"] not in NAMES or d["variant"] not in VARIANTS:
        raise ValueError("wrong variant/scenario")
    for k in HEADER[2:]:
        if not d[k].isascii() or not d[k].isdecimal():
            raise ValueError("expected unsigned decimal: "+k)
        d[k]=int(d[k])
    return d

def check_group(rows):
    if len(rows)!=len(VARIANTS) or {r["variant"] for r in rows}!=set(VARIANTS):
        raise ValueError("missing/duplicate mode")
    ref=rows[0]
    for r in rows:
        if (r["scenario"],r["repetition"])!=(ref["scenario"],ref["repetition"]):
            raise ValueError("mismatched trace")
        if any(r[k]!=ref[k] for k in STABLE):
            raise ValueError("native parity/allocator mismatch")
        if r["queries"]!=192 or r["owners"]<8 or r["canonical_buckets"]<1 or r["owner_visits"]<1:
            raise ValueError("invalid frozen workload")
        if r["variant"].startswith("adaptive_"):
            if r["adaptive_skip"]+r["adaptive_find"]!=r["queries"]:
                raise ValueError("adaptive dispatch coverage mismatch")
            if r["scenario"] in ("short_l1_crossing","long_single_bucket") and r["adaptive_skip"]:
                raise ValueError("short/single-bucket query unexpectedly skipped")
        elif r["adaptive_skip"] or r["adaptive_find"]:
            raise ValueError("nonadaptive dispatcher counted")

def write_csv(path,fields,rows):
    with Path(path).open("w",newline="",encoding="utf-8") as f:
        writer=csv.DictWriter(f,fieldnames=fields,lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

def main(argv):
    p=argparse.ArgumentParser()
    p.add_argument("binary")
    p.add_argument("raw_csv")
    p.add_argument("summary_csv")
    args=p.parse_args(argv)
    records=[]
    for scenario in range(len(NAMES)):
        for rep in range(3):
            group=[]
            # Rotate order for runner noise; all variants are independent fresh processes.
            for mode in (list(range(6)) if rep%2==0 else list(range(5,-1,-1))):
                process=subprocess.run([args.binary,str(scenario),str(mode),str(rep)],
                     capture_output=True,text=True,check=False)
                if process.returncode:
                    raise RuntimeError(f"oracle {scenario}/{mode}/{rep} failed: {process.stderr}")
                row=parse(process.stdout)
                if (row["scenario"],row["variant"],row["repetition"])!=(
                    NAMES[scenario],VARIANTS[mode],rep):
                    raise ValueError("subprocess mislabeled")
                group.append(row)
            check_group(group)
            records.extend(group)
    for scenario in NAMES:
        checks={r["native_checksum"] for r in records if r["scenario"]==scenario}
        if len(checks)!=1:
            raise ValueError("cross-repeat checksum instability")
    for mode in VARIANTS[2:]:
        if not any(r["adaptive_skip"] for r in records if r["variant"]==mode):
            raise ValueError("adaptive skip path never covered: "+mode)
        if not any(r["adaptive_find"] for r in records if r["variant"]==mode):
            raise ValueError("adaptive find path never covered: "+mode)
    summary=[]
    for scenario in NAMES:
        legacy=[r for r in records if r["scenario"]==scenario and r["variant"]==VARIANTS[0]]
        for variant in VARIANTS[1:]:
            trial=[r for r in records if r["scenario"]==scenario and r["variant"]==variant]
            for metric in ("registration_ns","query_ns","churn_ns",
                           "post_query_ns","new_requests"):
                before=statistics.median(x[metric] for x in legacy)
                after=statistics.median(x[metric] for x in trial)
                summary.append({"scenario":scenario,"variant":variant,"metric":metric,
                    "legacy_median":int(before),"candidate_median":int(after),
                    "ratio_legacy_over_candidate":f"{before/after:.6f}" if after else "NA",
                    "selected_skip_queries":int(statistics.median(x["adaptive_skip"] for x in trial)),
                    "selected_find_queries":int(statistics.median(x["adaptive_find"] for x in trial)),
                    "evidence":"SYNTHETIC_CPU_ONLY"})
    write_csv(args.raw_csv,HEADER,records)
    write_csv(args.summary_csv,("scenario","variant","metric","legacy_median",
      "candidate_median","ratio_legacy_over_candidate","selected_skip_queries",
      "selected_find_queries","evidence"),summary)
    print("S4_B5B_ADAPTIVE_PASS,scenarios,6,matches,18,processes,108,"
          "oracle,NATIVE_SLOT_ID_PAGE_MAJOR,production_default,OFF,"
          "evidence,SYNTHETIC_CPU_ONLY")
    return 0

if __name__=="__main__":
    try:sys.exit(main(sys.argv[1:]))
    except (ValueError,RuntimeError,OSError) as exc:
        print("S4_B5B_REJECT: "+str(exc),file=sys.stderr)
        sys.exit(1)
