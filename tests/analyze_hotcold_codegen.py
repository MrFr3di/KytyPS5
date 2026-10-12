#!/usr/bin/env python3
"""S4-B5-E: Windows COFF static codegen audit; never a cycle/FPS measurement."""
from __future__ import annotations
import argparse
import csv
import hashlib
from pathlib import Path
import re
import sys
SYMBOLS=("KytyB5ELegacy","KytyB5EInline256","KytyB5EColdSkip",
         "KytyB5EHotCold256","KytyB5EFullDispatcher256")
FUNC=re.compile(r"(?m)^\s*[0-9a-fA-F]+\s+<([^>]+)>:\s*$")
INSTR=re.compile(r"(?m)^\s*[0-9a-fA-F]+:\s+((?:[0-9a-fA-F]{2}(?:\s+|$))+)")
def verify(text):
    if len(text)<200 or not text.isascii():
        raise ValueError("missing/non-ASCII disassembly")
    for symbol in SYMBOLS:
        if not re.search(r"\b"+re.escape(symbol)+r"\b",text):
            raise ValueError("missing COFF symbol "+symbol)
    hits=list(FUNC.finditer(text))
    metrics={}
    for i,match in enumerate(hits):
        name=match.group(1)
        if name not in SYMBOLS:continue
        chunk=text[match.end():hits[i+1].start() if i+1<len(hits) else len(text)]
        byte_tokens=INSTR.findall(chunk)
        if not byte_tokens:continue
        size=sum(len(re.findall(r"\b[0-9a-fA-F]{2}\b",tok)) for tok in byte_tokens)
        metrics[name]=(len(byte_tokens),size)
    return metrics
def main(argv):
    p=argparse.ArgumentParser()
    p.add_argument("disassembly")
    p.add_argument("summary_csv")
    p.add_argument("--tool",choices=("llvm-objdump","dumpbin"),required=True)
    a=p.parse_args(argv)
    data=Path(a.disassembly).read_text(encoding="utf-8",errors="replace")
    metrics=verify(data)
    sha=hashlib.sha256(data.encode("utf-8")).hexdigest()
    with Path(a.summary_csv).open("w",encoding="utf-8",newline="") as fp:
        w=csv.writer(fp,lineterminator="\n")
        w.writerow(("symbol","static_instructions","static_code_bytes","disasm_sha256",
                    "tool","evidence"))
        for s in SYMBOLS:
            n,b=metrics.get(s,("NA","NA"))
            w.writerow((s,n,b,sha,a.tool,"STATIC_STANDALONE_PROBE_NOT_CYCLES"))
    if a.tool=="llvm-objdump" and len(metrics)!=len(SYMBOLS):
        raise ValueError("llvm-objdump symbol sections could not be accounted")
    print("S4_B5E_CODEGEN_PASS,functions,5,metric_sections,"+str(len(metrics))+
          ",disasm_sha256,"+sha+",not_production_tu,1")
    return 0
if __name__=="__main__":
    try:sys.exit(main(sys.argv[1:]))
    except (OSError,ValueError) as exc:
        print("S4_B5E_CODEGEN_REJECT: "+str(exc),file=sys.stderr)
        sys.exit(1)
