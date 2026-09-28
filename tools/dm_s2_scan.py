#!/usr/bin/env python3
"""Offline .dm_s2 inspector.

Answers, without launching the game:

  1. What TLVs does this demo actually contain?
  2. What is in the recorded gamestate — which configstrings, and are any of them
     in a NetConstStrings range (where they do not belong)?
  3. Does the demo carry a StreamSync load request (server-command opcode 98)?

Question 3 checks for the StreamSync load request handled by sub_193B20,
which determines the next step in the viewmodel investigation.

Usage:
    python tools/dm_s2_scan.py <demo.dm_s2> [--strings] [--max-strings N]
    python tools/dm_s2_scan.py            # picks the newest demo it can find

Read-only. Never writes to the demo.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

# ---------------------------------------------------------------------------
# Container format. Mirrors demo_utils.cpp write_id_and_size / write_*.
#   id byte: low 5 bits = id, 0x80 = "size is one byte" flag
#   size:    1 byte when flagged, else u32 LE
# ---------------------------------------------------------------------------

TLV_NAMES = {
    0: "mod_header",
    1: "map_header",
    2: "network_data",
    3: "predicted_data",
    4: "update_gamestate_data",
    5: "gen_header",
    6: "gen_footer",
    7: "gamestate_message",
    31: "eof",
}

MAX_CONFIGSTRINGS = 6994  # S2 0x1B62, from CL_GetConfigString @ 0x79160

# NetConstStrings ranges, decoded from the S2 static table at 0xB2E590.
# (type, start, count) -> covers [start, start+count). Strings in these ranges
# come from fastfiles and are NOT replicated, so a demo never needs to carry them.
NCS_RANGES = [
    (0, 1270, 2048), (1, 4472, 511), (2, 1224, 32),  (3, 3318, 64),
    (4, 3382, 1024), (5, 570, 650),  (6, 6994, 0),   (7, 6994, 0),
    (8, 4406, 16),   (9, 4422, 50),  (10, 4983, 64), (11, 5047, 3),
    (12, 5050, 15),  (13, 6418, 14), (14, 6403, 15), (15, 1266, 4),
    (16, 5065, 826), (17, 5891, 512), (18, 315, 255), (19, 6432, 511),
    (20, 6994, 0),   (21, 6994, 0),  (22, 6944, 32), (23, 6994, 0),
    (24, 10, 128),   (25, 6994, 0),
]

# Server-command framing, from CG_ExecuteServerCommand @ 0x4373F0 and
# sub_431B20 @ 0x431B20:
#   cmd[0]   == 'Z' (0x5A)  -> binary command path
#   cmd[2:4] == u16 payload length
#   cmd[4]   == first opcode byte; sub_431B20 switches on (opcode - 45)
#   case 53  -> sub_193B20  == StreamSync load request, so opcode == 98
STREAMSYNC_OPCODE = 98
OPCODE_NAMES = {
    45: "sub_43F700",
    49: "entity fx",
    59: "sub_181F00",
    71: "sub_437160",
    79: "sub_16A1B0/sub_16A450",
    98: "sub_193B20  << STREAMSYNC LOAD REQUEST",
}


def ncs_type_for(index: int):
    for ncs_type, start, count in NCS_RANGES:
        if count and start <= index < start + count:
            return ncs_type
    return None


def iter_tlvs(blob: bytes):
    """Yield (offset, tlv_id, payload). Stops on malformed data."""
    off = 0
    n = len(blob)
    while off < n:
        head = blob[off]
        tlv_id = head & 0x1F
        one_byte = bool(head & 0x80)
        if one_byte:
            if off + 2 > n:
                return
            size = blob[off + 1]
            body = off + 2
        else:
            if off + 5 > n:
                return
            size = struct.unpack_from("<I", blob, off + 1)[0]
            body = off + 5
        if size > len(blob) or body + size > n:
            print(f"  !! truncated TLV id={tlv_id} at 0x{off:X} "
                  f"(claims {size} bytes, {n - body} remain)")
            return
        yield off, tlv_id, blob[body:body + size]
        if tlv_id == 31:  # eof
            return
        off = body + size


def decode_gamestate(payload: bytes):
    """update_gamestate_data (id=4). Layout from demo_utils.cpp:516-535."""
    if len(payload) < 12:
        print("  !! gamestate payload too small")
        return None
    svr_cmd_seq = struct.unpack_from("<I", payload, 0)[0]
    packed = struct.unpack_from("<Q", payload, 4)[0]
    sd_size = packed & 0xFFFFF                 # bits  0..19
    so_size = (packed >> 20) & 0xFFFF          # bits 20..35
    compressed = (packed >> 63) & 1            # bit 63

    off = 12
    sd_blob = payload[off:off + sd_size]
    off += sd_size
    so_blob = payload[off:off + so_size]
    off += so_size
    tail = payload[off:]

    print(f"  svr_cmd_seq        = {svr_cmd_seq}")
    print(f"  string_data  bytes = {sd_size} (stored){' zlib' if compressed else ''}")
    print(f"  string_offsets     = {so_size} (stored)")
    print(f"  persistent tail    = {len(tail)} bytes")

    if compressed:
        try:
            string_data = zlib.decompress(sd_blob)
            string_offsets = zlib.decompress(so_blob)
        except zlib.error as exc:
            print(f"  !! zlib error: {exc}")
            return None
    else:
        string_data, string_offsets = sd_blob, so_blob

    print(f"  string_data  inflated = {len(string_data)} bytes")
    expect = MAX_CONFIGSTRINGS * 4
    print(f"  string_offsets inflated = {len(string_offsets)} bytes (expect {expect})")
    if len(string_offsets) != expect:
        got_entries = len(string_offsets) // 4
        delta = len(string_offsets) - expect
        print(f"  !! MISMATCH: {got_entries} offset entries, engine bound is "
              f"{MAX_CONFIGSTRINGS} ({delta:+d} bytes)")
        print("     The engine lays out stringOffsets[] immediately followed by")
        print("     stringData (GS_STRING_DATA_OFF 0x6D88 == 6994*4), so restoring an")
        print("     oversized offsets blob writes past the array and into the head of")
        print("     stringData. Check demo_game.hpp MAX_CONFIGSTRINGS against the")
        print("     build that recorded this demo before trusting its gamestate.")

    count = min(MAX_CONFIGSTRINGS, len(string_offsets) // 4)
    offsets = struct.unpack_from(f"<{count}i", string_offsets, 0)

    entries = []
    for idx, sofs in enumerate(offsets):
        if sofs <= 0 or sofs >= len(string_data):
            continue
        end = string_data.find(b"\0", sofs)
        raw = string_data[sofs:end if end >= 0 else len(string_data)]
        if raw:
            entries.append((idx, raw.decode("utf-8", "replace")))
    return svr_cmd_seq, entries


def report_configstrings(entries, show: bool, limit: int):
    replicated, misplaced = [], []
    for idx, value in entries:
        if ncs_type_for(idx) is None:
            replicated.append((idx, value))
        else:
            misplaced.append((idx, value))

    print(f"  non-empty configstrings = {len(entries)}"
          f"  (replicated {len(replicated)}, in-NCS-range {len(misplaced)})")
    if misplaced:
        print("  NOTE: indices inside a NetConstStrings range carry data. The engine")
        print("        asserts these should be empty, so treat them as suspicious.")

    if show:
        for label, rows in (("replicated", replicated), ("in NCS range", misplaced)):
            if not rows:
                continue
            print(f"  --- {label} ---")
            for idx, value in rows[:limit]:
                tag = ncs_type_for(idx)
                suffix = f"  [ncs type {tag}]" if tag is not None else ""
                print(f"    [{idx:5}] {value!r}{suffix}")
            if len(rows) > limit:
                print(f"    ... {len(rows) - limit} more (raise --max-strings)")


def scan_server_commands(chunks):
    """Heuristic scan for the 'Z' binary server-command framing.

    HEURISTIC, and deliberately labelled as such: the payloads are the raw
    server-message bytes, which are bit-packed, so a rigorous decode needs the
    netfield tables. This looks for the literal framing instead:
        [0]=0x5A 'Z'   [2:4]=u16 length (plausible)   [4]=opcode
    False positives are possible; a HIT on opcode 98 is still strong evidence,
    and a total absence of any 'Z' framing is itself informative.
    """
    hist, streamsync_hits = {}, []
    for tlv_off, data in chunks:
        for i in range(len(data) - 5):
            if data[i] != 0x5A:
                continue
            length = struct.unpack_from("<H", data, i + 2)[0]
            if not (1 <= length <= 1024):
                continue
            opcode = data[i + 4]
            hist[opcode] = hist.get(opcode, 0) + 1
            if opcode == STREAMSYNC_OPCODE:
                streamsync_hits.append((tlv_off, i, length))
    return hist, streamsync_hits


def find_newest_demo() -> Path | None:
    roots = [
        Path(r"E:\SteamLibrary\steamapps\common\Call of Duty WWII\demos"),
        Path.cwd() / "demos",
    ]
    found = [p for r in roots if r.is_dir() for p in r.glob("*.dm_s2")]
    return max(found, key=lambda p: p.stat().st_mtime) if found else None


def main() -> int:
    ap = argparse.ArgumentParser(description="Offline .dm_s2 inspector")
    ap.add_argument("demo", nargs="?", type=Path, help="path to a .dm_s2 file")
    ap.add_argument("--strings", action="store_true",
                    help="list the recorded configstrings")
    ap.add_argument("--max-strings", type=int, default=60,
                    help="cap per section when listing (default 60)")
    args = ap.parse_args()

    path = args.demo or find_newest_demo()
    if not path:
        print("no demo given and none found; pass a path explicitly")
        return 2
    if not path.is_file():
        print(f"not a file: {path}")
        return 2

    blob = path.read_bytes()
    print(f"=== {path}")
    print(f"    {len(blob)} bytes\n")

    counts, net_chunks, gs_seen = {}, [], False
    for off, tlv_id, payload in iter_tlvs(blob):
        name = TLV_NAMES.get(tlv_id, f"unknown({tlv_id})")
        entry = counts.setdefault(name, [0, 0])
        entry[0] += 1
        entry[1] += len(payload)

        if tlv_id in (2, 7):
            net_chunks.append((off, payload))
        elif tlv_id in (0, 1, 5) and payload:
            text = payload.rstrip(b"\0").replace(b"\0", b" | ")
            print(f"[{name}] {text.decode('utf-8', 'replace')}")
        elif tlv_id == 4 and not gs_seen:
            gs_seen = True
            print(f"[{name}] @0x{off:X}")
            decoded = decode_gamestate(payload)
            if decoded:
                report_configstrings(decoded[1], args.strings, args.max_strings)
            print()

    print("--- TLV inventory ---")
    for name, (n, total) in sorted(counts.items()):
        print(f"  {name:22} x{n:<6} {total} bytes")
    if not gs_seen:
        print("\n  !! no update_gamestate_data TLV in this demo")

    print("\n--- server-command scan (HEURISTIC, see docstring) ---")
    hist, hits = scan_server_commands(net_chunks)
    if not hist:
        print("  no 'Z' binary-command framing found in any network payload")
    else:
        for opcode in sorted(hist):
            label = OPCODE_NAMES.get(opcode, "")
            print(f"  opcode {opcode:3} x{hist[opcode]:<6} {label}")

    net_bytes = sum(len(p) for _, p in net_chunks)
    total_hits = sum(hist.values())
    print()
    if hits:
        print(f"  VERDICT: {len(hits)} candidate StreamSync load request(s) "
              f"(opcode {STREAMSYNC_OPCODE}) present.")
        print("           Investigate why it is not taking effect — check the two gates")
        print("           in sub_193B20 before anything else.")
        for tlv_off, i, length in hits[:10]:
            print(f"             TLV @0x{tlv_off:X} +{i} len={length}")
    elif total_hits < 32 or (net_bytes and total_hits / net_bytes < 1e-4):
        # The wire format is bit-packed, so reliable commands are NOT stored as
        # byte-aligned 'Z' records. If the framing barely matches anything, the
        # scan did not find server commands of ANY kind and proves nothing about
        # opcode 98 specifically. Say so rather than reporting a false negative.
        print(f"  VERDICT: INCONCLUSIVE. Only {total_hits} 'Z' framing hits across "
              f"{net_bytes} bytes of")
        print("           network payload — noise level. The scan is not detecting")
        print("           server commands of any kind, so it cannot speak to whether")
        print("           opcode 98 is present. The payloads are bit-packed, so the")
        print("           literal byte framing does not survive on the wire.")
        print("           To answer this properly you need a real bitstream reader for")
        print("           S2's server-message format, not a byte scan.")
    else:
        print(f"  VERDICT: no opcode {STREAMSYNC_OPCODE} among {total_hits} framing hits.")
        print("           Weak evidence only — absence under a heuristic is not proof.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
