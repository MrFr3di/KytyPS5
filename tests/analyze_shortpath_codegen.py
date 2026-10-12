#!/usr/bin/env python3
"""S4-B5-D: verify actual Windows COFF disassembly contains four C ABI probes.

This is STATIC codegen provenance, not an instruction/cycle or FPS measurement.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
from pathlib import Path
import re
import sys

SYMBOLS=("KytyB5DDirectFind","KytyB5DCanonicalLegacy",
         "KytyB5DPageOnly256","KytyB5DManual256")

def verify(disassembly: str) -> dict[str, int]:
    if len(disassembly)<200 or not disassembly.isascii():
        raise ValueError("empty/non-ASCII disassembly")
    positions={}
    for symbol in SYMBOLS:
        matched=list(re.finditer(r"\b"+re.escape(symbol)+r"\b",disassembly))
        if not matched:
            raise ValueError("missing machine-code function "+symbol)
        positions[symbol]=len(matched)
    if len(set(SYMBOLS))!=len(SYMBOLS):
        raise ValueError("duplicate probe symbol")
    return positions

def main(argv):
    p=argparse.ArgumentParser()
    p.add_argument("disassembly")
    p.add_argument("summary_csv")
    p.add_argument("--tool",choices=("llvm-objdump","dumpbin"),required=True)
    o=p.parse_args(argv)
    source=Path(o.disassembly).read_text(encoding="utf-8",errors="replace")
    count=verify(source)
    sha=hashlib.sha256(source.encode("utf-8")).hexdigest()
    with Path(o.summary_csv).open("w",encoding="utf-8",newline="") as out:
        writer=csv.writer(out,lineterminator="\n")
        writer.writerow(("symbol","mentions","disassembly_sha256","disassembler","evidence"))
        for symbol in SYMBOLS:
            writer.writerow((symbol,count[symbol],sha,o.tool,"STATIC_PROBE_NOT_CYCLES"))
    print("S4_B5D_CODEGEN_PASS,functions,4,tool,"+o.tool+
          ",disasm_sha256,"+sha+",not_production_tu,1")
    return 0
if __name__=="__main__":
    try:sys.exit(main(sys.argv[1:]))
    except (ValueError,OSError) as ex:
        print("S4_B5D_CODEGEN_REJECT: "+str(ex),file=sys.stderr)
        sys.exit(1)
