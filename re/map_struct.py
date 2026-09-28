#!/usr/bin/env python3
"""
map_struct.py -- derive a struct's field offsets from an accessor's callers.

Generalisation of map_centity.py. Point it at any function that returns a
pointer to the struct; every dereference of that return value across all callers
is a field access, and the SET of offsets touched is the used layout.

    python re/map_struct.py --accessor 0x13A00 --max 1808        # centity_s
    python re/map_struct.py --accessor 0x15330 --max 20000       # cg_s head
                                                                 # (= playerState,
                                                                 #  which sits at cg+0)

TRANSPORT CONSTRAINTS
    - the MCP plugin enforces a hard 60 s internal timeout -> batch;
    - replies truncate past ~60 KB -> run the regex INSIDE IDA and return counts
      only, never pseudocode.
"""

from __future__ import annotations

import argparse
import ast
import collections
import json
import sys
import urllib.request
from pathlib import Path

RE_DIR = Path(__file__).resolve().parent

CALLERS = r'''
def main():
    import idautils, ida_funcs
    out = sorted({ida_funcs.get_func(x.frm).start_ea
                  for x in idautils.XrefsTo(ACC, 0)
                  if ida_funcs.get_func(x.frm)})
    return [hex(a) for a in out]
main()
'''

SCAN = r'''
def main():
    import ida_hexrays, ida_funcs, re

    ASSIGN = re.compile(r"(\w+)\s*=\s*(?:\([^)]*\))?\s*" + ACCNAME + r"\(")
    INLINE = re.compile(ACCNAME + r"\([^;]*?\)\s*\+\s*(\d+)")

    hits, funcs, ok = {}, {}, 0
    for h in BATCH:
        ea = int(h, 16)
        try:
            code = str(ida_hexrays.decompile(ea))
        except Exception:
            continue
        ok += 1
        nm = ida_funcs.get_func_name(ea)

        def note(off, ty):
            if 0 <= off <= MAXOFF:
                hits.setdefault(off, {})
                hits[off][ty] = hits[off].get(ty, 0) + 1
                funcs.setdefault(off, set()).add(nm)

        for off in INLINE.findall(code):
            note(int(off), "(inline)")
        for var in set(ASSIGN.findall(code)):
            d = re.compile(r"\*\(\s*([_A-Za-z0-9]+)\s*\*\s*\)\(\s*"
                           + re.escape(var) + r"\s*\+\s*(\d+)\s*\)")
            for ty, off in d.findall(code):
                note(int(off), ty)
            b = re.compile(r"\*\(\s*([_A-Za-z0-9]+)\s*\*\s*\)" + re.escape(var) + r"\b")
            for ty in b.findall(code):
                note(0, ty)

    return {"ok": ok,
            "hits": {str(k): v for k, v in hits.items()},
            "funcs": {str(k): sorted(v)[:6] for k, v in funcs.items()}}
main()
'''

WIDTH = {"_BYTE": 1, "char": 1, "unsigned __int8": 1, "__int8": 1, "bool": 1,
         "_WORD": 2, "__int16": 2, "unsigned __int16": 2,
         "_DWORD": 4, "int": 4, "unsigned int": 4, "float": 4,
         "_QWORD": 8, "__int64": 8, "double": 8}


def rpc(port, code, timeout=600):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": "py_eval", "arguments": {"code": code}}}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}/mcp", body,
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        text = json.load(r)["result"]["content"][0]["text"]
    env = json.loads(text)
    if env.get("stderr"):
        raise RuntimeError(env["stderr"][:300])
    if not env.get("result"):
        return None
    try:
        return ast.literal_eval(env["result"])
    except (ValueError, SyntaxError):
        # the server str()s the return value, so a plain string comes back bare
        return env["result"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=13337)
    ap.add_argument("--accessor", required=True)
    ap.add_argument("--accname", default=None, help="name as it appears in pseudocode")
    ap.add_argument("--max", type=int, required=True)
    ap.add_argument("--batch", type=int, default=25)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    acc = int(a.accessor, 16)
    accname = a.accname or rpc(a.port,
        f"def main():\n import ida_funcs\n return ida_funcs.get_func_name({acc})\nmain()")
    print(f"accessor {a.accessor} = {accname}, offsets <= {a.max}")

    callers = rpc(a.port, f"ACC = {acc}\n" + CALLERS)
    print(f"callers: {len(callers)}")

    hits = collections.defaultdict(collections.Counter)
    funcs = collections.defaultdict(set)
    done = 0
    for i in range(0, len(callers), a.batch):
        batch = callers[i:i + a.batch]
        try:
            res = rpc(a.port, f"BATCH = {json.dumps(batch)}\nMAXOFF = {a.max}\n"
                              f"ACCNAME = {accname!r}\n" + SCAN)
        except Exception as e:
            print(f"  batch {i//a.batch+1} failed: {e}")
            continue
        done += res["ok"]
        for off, tys in res["hits"].items():
            for ty, n in tys.items():
                hits[int(off)][ty] += n
        for off, fs in res["funcs"].items():
            funcs[int(off)].update(fs)
        if (i // a.batch) % 4 == 0:
            print(f"  {done}/{len(callers)} decompiled, {len(hits)} offsets")

    rows = []
    for off in sorted(hits):
        c = hits[off]
        ty = c.most_common(1)[0][0]
        rows.append({"offset": off, "hex": hex(off), "type": ty,
                     "width": WIDTH.get(ty, 0), "hits": sum(c.values()),
                     "types": dict(c), "funcs": sorted(funcs[off])[:6]})
    Path(a.out).write_text(json.dumps(rows, indent=1))
    print(f"\n{len(rows)} distinct offsets from {done} functions -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
