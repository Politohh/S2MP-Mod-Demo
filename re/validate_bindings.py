#!/usr/bin/env python3
"""
validate_bindings.py -- cross-check the {name -> function} binding tables scraped
from S2's .rdata against the game's OWN compiled LUI Lua.

WHY THIS EXISTS
    The binding tables are already the strongest evidence tier: the binary itself
    pairs a name string with a function pointer, so the mapping is not inferred.
    But the *interpretation* -- "these are Havok Script library tables, registered
    under a Lua namespace" -- is a claim, and it deserves an independent source.

    The 941 dumped LUI files are that source. They are HKS BYTECODE, not text, so
    grep finds nothing. Their constant tables, however,
    hold every identifier the script references as plain NUL-terminated ASCII.
    If `CanHostServer` appears there, the name is corroborated by an artifact that
    has nothing to do with the .rdata scrape.

    A name that fails to appear is NOT evidence of a wrong name -- the shipped Lua
    simply may not call it. So this measures corroboration, never refutation.

USAGE
    python re/validate_bindings.py <dump_dir> <bindings.json>
"""

from __future__ import annotations

import collections
import json
import re
import sys
from pathlib import Path

# HKS bytecode stores identifiers as plain NUL-terminated ASCII in the constant
# table. 2+ chars keeps the noise down without dropping real short names.
TOKEN = re.compile(rb"[A-Za-z_][A-Za-z0-9_]{1,63}")


def harvest(dump_dir: Path) -> collections.Counter:
    """Every printable identifier-shaped run across every dumped script."""
    seen: collections.Counter = collections.Counter()
    files = 0
    for p in dump_dir.rglob("*"):
        if not p.is_file():
            continue
        try:
            blob = p.read_bytes()
        except OSError:
            continue
        files += 1
        for m in TOKEN.findall(blob):
            seen[m.decode("ascii")] += 1
    print(f"  scanned {files} files, {len(seen)} distinct identifiers")
    return seen


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    dump_dir, bindings = Path(sys.argv[1]), Path(sys.argv[2])
    print(f"harvesting identifiers from {dump_dir} ...")
    ident = harvest(dump_dir)

    entries = json.loads(bindings.read_text())
    by_table: dict[str, list] = collections.defaultdict(list)
    for e in entries:
        by_table[e["table"]].append(e)

    print(f"\n{'table':>12} {'n':>5} {'confirmed':>10} {'rate':>6}  registrar")
    print("-" * 74)

    tot = hit = 0
    rows = []
    for tbl, es in sorted(by_table.items(), key=lambda kv: -len(kv[1])):
        n = len(es)
        c = sum(1 for e in es if e["binding"] in ident)
        tot += n
        hit += c
        rows.append((tbl, n, c, es[0]["registrar"]))

    for tbl, n, c, reg in rows:
        rate = f"{100 * c / n:.0f}%"
        print(f"{tbl:>12} {n:>5} {c:>10} {rate:>6}  {reg}")

    print("-" * 74)
    print(f"{'TOTAL':>12} {tot:>5} {hit:>10} {100 * hit / tot:>5.0f}%")

    # Names the scripts never mention -- reported so the gap is visible, not hidden.
    missing = [e["binding"] for e in entries if e["binding"] not in ident]
    print(f"\nnot referenced by any shipped script: {len(missing)}")
    print("  sample:", ", ".join(sorted(set(missing))[:20]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
