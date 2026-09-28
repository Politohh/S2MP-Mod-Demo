#!/usr/bin/env python3
"""
s2_capture_diff.py -- why does the engine's own recorder produce online demos that
raise Com_Error "4780", when the SAME online data captured by our theater replays
fine?

THE ELIMINATION SO FAR (measured)
---------------------------------------------------
    public match, captured at CL_ParseServerMessage entry, transcoded  -> PLAYS
    public match, captured by CL_Demo_StartRecord's recorder           -> 4780
    local/bot,    captured by CL_Demo_StartRecord's recorder           -> PLAYS

Same playback code, same recorder code, both public matches. Ruled out along the
way: missing baselines (delta bit-counts come from the changed-field mask in the
stream, not from baseline VALUES, so a bad baseline gives wrong values and never a
non-increasing index), a delta against a missing baseline (every first snapshot is
deltaNum=0 FULL), netfield table mismatch (both demo groups point at identical
tables and the version gate would raise "432"), and the gamestate itself (decodes
cleanly: map, gametype, serverId, 119 configstrings).

What is left is what bytes each recorder actually captured. This tool answers that
by diffing them directly, which needs no further inference.

HOW TO PRODUCE THE INPUTS
-------------------------
In ONE online match, with both recorders running:

    demo_start          <- arms the theater (.dm_s2); also sets demoautorecord 1
    (demo_record is already ON -- the engine's native recorder)
    ... play, then leave the match ...

That yields  demos/<map>.NNNN.dm_s2  and  main/demo/x????_????????.demo  of the SAME
match. Then:

    python tools/s2_capture_diff.py <native.demo> <ours.dm_s2>

WHAT IT REPORTS
---------------
Messages are aligned by the snapshot's messageNum (which both carry and neither
recorder invents), then the DECODED message bodies are compared byte for byte.

  * identical bytes -> the recorders captured the same thing, and the fault is in
    playback STATE rather than in the recording. That would redirect the whole
    investigation.
  * differing bytes -> the first differing offset, with context, names the defect
    outright.

Sizes are compared too: a systematic length difference (e.g. ours longer by N)
points at trailing bytes one side keeps and the other drops -- note
CL_ParseServerMessage calls sub_191760 only when `cursize - readcount >= 0x10`,
i.e. exactly when there ARE trailing bytes, which is a plausible online-only
condition.
"""

import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s2_huffman as huff
import s2_svc_walk
from s2_svc_walk import Msg, decode_payload


# --------------------------------------------------------------------------
#  .dm_s2 side
# --------------------------------------------------------------------------
def iter_tlvs(blob):
    off, n = 0, len(blob)
    while off < n:
        head = blob[off]
        tlv_id = head & 0x1F
        if head & 0x80:
            if off + 2 > n:
                return
            size = blob[off + 1]
            body = off + 2
        else:
            if off + 5 > n:
                return
            size = struct.unpack_from("<I", blob, off + 1)[0]
            body = off + 5
        if body + size > n:
            return
        yield tlv_id, blob[body:body + size]
        if tlv_id == 31:
            return
        off = body + size


def decode_body(raw, use_zlib):
    if use_zlib:
        try:
            return zlib.decompress(raw)
        except zlib.error:
            return None
    return huff.huff_decode(s2_svc_walk.decode_payload.table, raw, len(raw), 0x20000)


def custom_messages(path):
    """{msgNum: (decoded body, raw len, entry readcount, useZlib)} from a .dm_s2."""
    out = {}
    blob = open(path, "rb").read()
    for tlv_id, payload in iter_tlvs(blob):
        if tlv_id not in (2, 7) or len(payload) < 8:
            continue
        flags = struct.unpack_from("<I", payload, 4)[0]
        rc = flags & 0x7FFFFFFF
        use_zlib = bool(flags >> 31)
        data = payload[8:]
        if rc + 2 > len(data):
            continue
        declared = struct.unpack_from("<h", data, rc)[0]
        raw = data[rc + 2: rc + 2 + declared] if 0 < declared <= len(data) - rc - 2 \
            else data[rc:]
        body = decode_body(raw, use_zlib)
        if not body:
            continue
        mn = snapshot_msgnum(body)
        if mn is not None:
            out.setdefault(mn, (body, len(raw), rc, use_zlib))
    return out


def native_messages(path):
    """{msgNum: (decoded body, raw len, -, useZlib)} from a native .demo."""
    out = {}
    blob = open(path, "rb").read()
    for _i, _o, _s, payload in huff.packets(blob):
        body, z = decode_payload(payload)
        if not body:
            continue
        mn = snapshot_msgnum(body)
        if mn is not None:
            out.setdefault(mn, (body, len(payload), None, z))
    return out


def snapshot_msgnum(body):
    """messageNum of the first svc 6 in this message, or None.

    Field widths are the VALIDATED ones from s2_svc_walk: long serverTime,
    long messageNum, BYTE deltaNum, BYTE snapFlags. Reading deltaNum as a long
    produces garbage even for demos that play -- do not 'simplify' this.
    """
    m = Msg(body)
    for _ in range(64):
        op = m.read_bits(4)
        if op == 7 or m.overflow:
            return None
        if op == 2:
            m.read_long(); m.read_string(1024)
        elif op == 3:
            m.read_long()
            ln = m.read_short()
            if ln <= 0 or ln > 0x3FC:
                return None
            m._take(ln)
        elif op == 6:
            m.read_long()
            return m.read_long()
        elif op == 0:
            return None
        else:
            return None
    return None


def hexdump(b, off, n=32):
    lo = max(0, off - 8)
    hi = min(len(b), off + n)
    chunk = b[lo:hi]
    return "@%d  %s" % (lo, " ".join("%02X" % c for c in chunk))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("native", help="main/demo/x????_????????.demo")
    ap.add_argument("custom", help="demos/<map>.NNNN.dm_s2")
    ap.add_argument("--max", type=int, default=6, help="messages to compare")
    a = ap.parse_args()

    if s2_svc_walk.decode_payload.table is None:
        s2_svc_walk.decode_payload.table = huff.load_table()

    nat = native_messages(a.native)
    cus = custom_messages(a.custom)
    print("native  %-40s %d snapshot messages" % (os.path.basename(a.native), len(nat)))
    print("custom  %-40s %d snapshot messages" % (os.path.basename(a.custom), len(cus)))

    common = sorted(set(nat) & set(cus))
    if not common:
        print("\nNO OVERLAPPING messageNum. The two files are not the same match, or one\n"
              "recorder started after the other stopped. Re-record with both armed in a\n"
              "single match.")
        print("  native msgNums: %s" % sorted(nat)[:12])
        print("  custom msgNums: %s" % sorted(cus)[:12])
        return 1

    print("\n%d messages present in BOTH (comparing first %d)\n"
          % (len(common), min(a.max, len(common))))

    same = diff = 0
    for mn in common[:a.max]:
        nb, nraw, _, nz = nat[mn]
        cb, craw, crc, cz = cus[mn]
        tag = "msgNum=%-6d native(decoded=%d raw=%d zlib=%d)  custom(decoded=%d raw=%d " \
              "entryReadcount=%s zlib=%d)" % (mn, len(nb), nraw, nz, len(cb), craw, crc, cz)
        if nb == cb:
            print("  SAME   %s" % tag)
            same += 1
            continue
        diff += 1
        first = next((i for i in range(min(len(nb), len(cb))) if nb[i] != cb[i]),
                     min(len(nb), len(cb)))
        print("  DIFFER %s" % tag)
        print("         first difference at byte %d of %d/%d"
              % (first, len(nb), len(cb)))
        print("         native %s" % hexdump(nb, first))
        print("         custom %s" % hexdump(cb, first))

    print("\n%d identical, %d differing" % (same, diff))
    if diff == 0:
        print(
            "VERDICT: both recorders captured the SAME BYTES. The 4780 is therefore NOT\n"
            "a capture-content problem -- it is playback STATE that differs between the\n"
            "two routes. Next: compare what each route leaves in clc/clientActive before\n"
            "the first snapshot (the transcoded demo leads with a text command, so the\n"
            "gamestate is the SECOND record; the native one leads with the gamestate).")
    else:
        print(
            "VERDICT: the recorders captured DIFFERENT bytes for the same message. The\n"
            "offset above is the defect. Check whether the difference is a prefix, a\n"
            "suffix (trailing bytes -- sub_191760 runs only when cursize-readcount >= 16),\n"
            "or a whole-body shift.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
