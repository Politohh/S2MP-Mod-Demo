"""Thin helpers for the S2 IDB currently served on port 11224.

⚠ THIS IS NOT THE AUTHORITATIVE IDB.  server_health reports
    idb   C:\\Users\\joshu\\OneDrive\\Documents\\s2x_dump.exe (1).i64
    input F:\\SteamLibrary\\...\\Call of Duty WWII\\s2x_dump.exe

The project's authoritative database is
    E:\\SteamLibrary\\steamapps\\common\\Call of Duty WWII\\s2x_dump.exe.i64
Reading the served database is fine: the binary is the same, and the string
at 0xBCE9E0 is "EXE_ERR_PROCESS_DEMO_FILE_FAILED", confirming the expected
address layout. Names applied here do not reach the E: copy.

Address convention, unchanged:  _b literal = IDA - 0x1000.
"""
from __future__ import annotations

import json
import re as _re
import urllib.request

PORT = 11224


def call(tool, args=None, timeout=600, port=PORT):
    payload = json.dumps({
        "jsonrpc": "2.0", "id": 1, "method": "tools/call",
        "params": {"name": tool, "arguments": args or {}},
    }).encode()
    req = urllib.request.Request(
        "http://127.0.0.1:%d/mcp" % port, data=payload,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = json.loads(r.read().decode("utf-8", "replace"))
    if "error" in body:
        raise RuntimeError(body["error"])
    content = body["result"].get("content", [])
    # Large results are truncated in-band with a URL to the full artifact.
    for c in content:
        t = c.get("text", "")
        if t.startswith("Output truncated"):
            m = _re.search(r"(http://\S+\.json)", t)
            if m:
                with urllib.request.urlopen(m.group(1), timeout=timeout) as r2:
                    return json.loads(r2.read().decode("utf-8", "replace"))
    out = []
    for c in content:
        t = c.get("text", "")
        try:
            out.append(json.loads(t))
        except Exception:
            out.append(t)
    return out[0] if len(out) == 1 else out


def dec(addr, addrs=True):
    """Decompile; returns the pseudocode text."""
    r = call("decompile", {"addr": addr if isinstance(addr, str) else hex(addr),
                           "include_addresses": addrs})
    if isinstance(r, dict):
        for k in ("pseudocode", "code", "text", "result"):
            if k in r:
                v = r[k]
                return v if isinstance(v, str) else json.dumps(v, indent=1)
    return r if isinstance(r, str) else json.dumps(r, indent=1)


def dis(addr, n=200, off=0):
    r = call("disasm", {"addr": addr if isinstance(addr, str) else hex(addr),
                        "max_instructions": n, "offset": off})
    if isinstance(r, dict):
        for k in ("disassembly", "instructions", "text", "result"):
            if k in r:
                v = r[k]
                if isinstance(v, list):
                    return "\n".join(
                        x if isinstance(x, str)
                        else "%s  %s" % (x.get("addr", ""), x.get("text", x))
                        for x in v)
                return v
    return r if isinstance(r, str) else json.dumps(r, indent=1)


def xrefs(addr, limit=200):
    return call("xrefs_to", {"addrs": [addr if isinstance(addr, str) else hex(addr)],
                             "limit": limit})


def fn(addr):
    """One function's record (name, size, ...)."""
    a = addr if isinstance(addr, str) else hex(addr)
    return call("func_query", {"queries": [{"addr": a}]})


def grep(text, pattern, ctx=0, flags=_re.I):
    """Pull only the interesting lines out of a big decompile (replies cap ~60 KB)."""
    lines = text.splitlines()
    hits, seen = [], set()
    for i, l in enumerate(lines):
        if _re.search(pattern, l, flags):
            for j in range(max(0, i - ctx), min(len(lines), i + ctx + 1)):
                if j not in seen:
                    seen.add(j)
                    hits.append("%5d  %s" % (j + 1, lines[j]))
    return "\n".join(hits)
