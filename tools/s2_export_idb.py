#!/usr/bin/env python3
"""
s2_export_idb.py -- dump the function graph of an open IDB to JSON, for diffing.

Runs the export INSIDE IDA via the ida-pro-mcp py_eval endpoint and has IDA write
the file locally, so a 60k-function graph never travels over JSON-RPC.

WHAT IT EXPORTS, and why only this
    size      the discriminator. The Steam and Store builds are the same source
              with the same compiler settings, so 21 of 22 known-good function
              pairs are IDENTICAL in size (measured 2026-08-15). That is what
              makes a cheap matcher viable at all.
    name      seeds the match wherever both IDBs already agree.
    callees   the call graph, built from XrefsTo at FUNCTION granularity rather
              than by walking every instruction -- O(functions x refs) instead of
              O(instructions), which matters on a 300 MB image.

⚠ DELIBERATELY NOT idautils.Strings(). It forces a full
strlist rebuild and TIMES OUT the MCP on this binary. Twice. String anchors, if
ever needed, come from reading .rdata bytes and searching them in Python.

USAGE
    python tools/s2_export_idb.py 13337 <out.json>      # Steam
    python tools/s2_export_idb.py 12346 <out.json>      # Xbox / Store
"""

from __future__ import annotations

import json
import sys
import urllib.request

# Built inside IDA. Returns a small summary; the bulk goes to disk.
EXPORT = r'''
import json, idautils, ida_funcs

funcs = {}
for fea in idautils.Functions():
    f = ida_funcs.get_func(fea)
    if not f:
        continue
    funcs[fea] = [f.size(), ida_funcs.get_func_name(fea), []]

# Call graph from the reverse direction: every xref that lands on a function
# START is a call/jump into it, and its owner is the caller. Far cheaper than
# walking instructions.
for fea in funcs:
    for xr in idautils.XrefsTo(fea, 0):
        c = ida_funcs.get_func(xr.frm)
        if c and c.start_ea in funcs and c.start_ea != fea:
            funcs[c.start_ea][2].append(fea)

for v in funcs.values():
    v[2] = sorted(set(v[2]))

with open(OUT, "w") as fh:
    json.dump({hex(k): v for k, v in funcs.items()}, fh)

{"functions": len(funcs),
 "edges": sum(len(v[2]) for v in funcs.values()),
 "out": OUT}
'''


def rpc(port: int, tool: str, args: dict, timeout: int = 900) -> str:
    body = json.dumps({
        "jsonrpc": "2.0", "id": 1, "method": "tools/call",
        "params": {"name": tool, "arguments": args},
    }).encode()
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}/mcp", body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        out = json.load(r)
    return out["result"]["content"][0]["text"]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    port, out = int(sys.argv[1]), sys.argv[2].replace("\\", "/")
    code = f'OUT = {out!r}\n' + EXPORT
    print(rpc(port, "py_eval", {"code": code}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
