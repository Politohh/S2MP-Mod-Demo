#!/usr/bin/env python3
"""
s2_dm_to_demo.py -- transcode our custom .dm_s2 into a NATIVE S2 .demo.

WHY
---
Native playback works (4/5 shipped demos) and the local viewmodel fix is
confirmed there, but S2 has NO native recorder: the only caller of
CL_Demo_WriteGameState is the clip path, which requires CL_IsDemoPlaying. So the
engine cannot produce a .demo from live play. Our theater CAN record. Converting
its output to the native container gets recording + native playback at once, and
lets the theater's playback half be retired rather than debugged.

Bonus: we write the container CORRECTLY. The shipped recorder has two proven
writer bugs (a missing 2-byte length prefix on packet 0, and CL_Demo_WriteGameState
emitting ONE bit where CL_ParseGamestate reads TWO). Files produced here have
neither, so they need no read-side compensation.

THE MAPPING (mechanical, not guessed)
-------------------------------------
.dm_s2 network_data (id 2) and gamestate_message (id 7) share one layout, from
demo_recording.cpp pack_network():

    [u32 seq][u32 flags: low31 = entry readcount, bit31 = useZlib][raw msg data]

`entry readcount` is where CL_ParseServerMessage was entered in live play, i.e.
the caller had already consumed the netchan header AND the sequence long (that is
how msg.useZlib was set before entry). From that offset the next read is
MSG_ReadShort(length), then the body.

A native type-2 packet is:

    [u8 type=2][u32 packetSeq][u32 payloadLen][payload]
    payload = [u32 msgSeq, bit31 = useZlib][u16 declaredLen][body]

So the payload is simply a fresh sequence long prepended to data[readcount:].

HEADER / FOOTER
---------------
Copied verbatim from a shipped demo of the SAME MAP. The header carries a
79,356-byte state blob at +0x1C that CL_Demo_Play_f memcpies to PlaybackData+76,
whose consumer is unknown -- copying a real one avoids having to synthesise it.
The footer is a complete NetConstStrings table, which resolves the model indices
snapshots carry; taking it from the same map keeps those indices meaningful.

    python tools/s2_dm_to_demo.py <in.dm_s2> <template.demo> <out.demo>

Validate the result with:
    python tools/s2_demo_read.py <out.demo>
"""

import argparse
import os
import re
import struct
import sys

HEADER_SIZE = 79384
DEMO_VERSION = 29


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


def to_payload(tlv_payload, fallback_seq):
    """One .dm_s2 message record -> (native type-2 payload, message sequence).

    THE SEQUENCE MUST BE THE REAL ONE, NOT A COUNTER.
    ------------------------------------------------
    An earlier version numbered packets 1,2,3,... That produced demos where
    `clc+0x138` (the message sequence CL_Demo_ProcessPacket_Type2_3 stores) read
    1,2,3,4 while the snapshot's own messageNum field inside the very same message
    read 29,30,31,32. Those are meant to be the same number, and the mismatch is a
    live-measured fact: a transcoded demo reached connstate 9 (PRIMED) and never 10
    (ACTIVE), because no snapshot ever validated.

    The real sequence survives in the capture. Live entry readcount is 8, i.e. the
    caller had consumed TWO longs before CL_ParseServerMessage:

        data[0:4]  message sequence            <- 29, 30, 31, ... (verified: matches
                                                  the snapshot messageNums exactly)
        data[4:8]  reliableAcknowledge | (useZlib << 31)

    Note the zlib flag rides on the SECOND long in the LIVE format but on the FIRST
    long in the DEMO format (CL_Demo_ProcessPacket_Type2_3 does
    `v6 = MSG_ReadLong(); clc[78] = v6 & 0x7FFFFFFF; useZlib = v6 >> 31`). So the
    demo payload gets one long carrying seq + zlib, and both live longs are stripped.

    The .dm_s2's own stored seq field is NOT usable: pack_network fills it via
    demo_game::clc_for(0), which disassembly shows reads the wrong global (0x1BD3C00
    instead of 0x1BD3D00). Measured: it is 0 in every record.
    """
    if len(tlv_payload) < 8:
        return None, None
    flags = struct.unpack_from("<I", tlv_payload, 4)[0]
    rc = flags & 0x7FFFFFFF
    use_zlib = (flags >> 31) & 1
    data = tlv_payload[8:]
    if rc > len(data):
        return None, None
    body = data[rc:]
    if len(body) < 2:
        return None, None
    # Sanity: the first thing the engine reads from here is the int16 length.
    declared = struct.unpack_from("<h", body, 0)[0]
    if declared <= 0 or declared > len(body) - 2:
        return None, None
    # Recover the true sequence from the stripped live header when it is there.
    msg_seq = fallback_seq
    if rc >= 4 and len(data) >= 4:
        msg_seq = struct.unpack_from("<I", data, 0)[0] & 0x7FFFFFFF
    seq_word = (msg_seq & 0x7FFFFFFF) | (use_zlib << 31)
    return struct.pack("<I", seq_word) + body, msg_seq


def to_archive(tlv_payload):
    """One .dm_s2 predicted_data record -> one native type-1 archive packet.

    WHY THIS EXISTS
    ---------------
    The first transcode emitted only type-2 snapshots, and the local player was
    rendered in the sea with frozen view angles. Snapshots carry the WORLD; the
    local player's predicted origin/velocity/viewangles live in the ARCHIVE ring
    at clientActive+0xA568, 188 bytes x 256, which only type-1 packets fill.
    Shipped demos are ~11 archives per snapshot.

    THE WIRE ORDER IS NOT THE STRUCT ORDER -- read from the disassembly of
    CL_Demo_ProcessPacket_Type1 @0x918640 (edx = size, rcx = destination):

        read  size  dest      field
          1     4   stack     ring index -> entry = base + 188*idx + 0xA568
          2    12   entry+4   origin
          3    12   entry+16  velocity
          4     4   entry+32  bobB          <- note: bobB BEFORE bobA
          5     4   entry+28  bobA
          6     4   entry+0   serverTime    <- sixth, not first
          7    12   entry+36  viewangles
          8     4   entry+48  extra0        <- then `cmp [rbp+30h], 0` gates
             if extra0 != 0:  4 -> +52, 2 -> +56
            128   entry+60  tail

        1 + 4 + 52 + 128 = 185, the proven archive packet size.

    Our recorder copies the engine's whole ClientArchiveEntry (`sample.cad =
    *entry`) with the engine's own ring index, so every field above is present.
    The 128-byte tail is NOT captured and is written as zeros.

    .dm_s2 predicted_data payload (60 B, demo_utils.cpp write_predicted_player):
        +0 id(1) +1 index(1) +2 serverTime(4) +6 origin(12) +18 velocity(12)
        +30 bobA(4) +34 bobB(4) +38 viewangles(12) +50 extra0(4) +54 w0,w1,w2(6)
    """
    if len(tlv_payload) < 60 or tlv_payload[0] != 0:      # id 0 == player
        return None
    p = tlv_payload
    extra0 = struct.unpack_from("<i", p, 50)[0]
    pkt = bytearray(b"\x01")
    pkt += struct.pack("<i", p[1])       # ring index (u8 in our container, 0..255)
    pkt += p[6:18]                       # origin      -> +4
    pkt += p[18:30]                      # velocity    -> +16
    pkt += p[34:38]                      # bobB        -> +32
    pkt += p[30:34]                      # bobA        -> +28
    pkt += p[2:6]                        # serverTime  -> +0
    pkt += p[38:50]                      # viewangles  -> +36
    pkt += p[50:54]                      # extra0      -> +48  (the gate)
    if extra0 != 0:
        # The engine reads 4 then 2 more bytes, which is exactly w0,w1,w2.
        pkt += p[54:58]                  #             -> +52
        pkt += p[58:60]                  #             -> +56
    pkt += b"\x00" * 128                 # tail        -> +60  (not captured)
    return bytes(pkt)


def capture_mapname(blob):
    """Map name of a .dm_s2, from its map_header record (TLV id 1).

    Falls back to scanning the gamestate record for an 'mp_*' token, so a capture
    written by an older recorder still resolves.
    """
    for tlv_id, payload in iter_tlvs(blob):
        if tlv_id == 1:
            txt = payload.split(b"\x00")[0]
            m = re.search(rb"mp_[a-z0-9_]+", txt)
            if m:
                return m.group(0).decode("ascii")
            m = re.search(rb"mp_[a-z0-9_]+", payload)
            if m:
                return m.group(0).decode("ascii")
    for tlv_id, payload in iter_tlvs(blob):
        if tlv_id == 7:
            m = re.search(rb"mp_[a-z0-9_]{3,}", payload)
            if m:
                return m.group(0).decode("ascii")
    return None


def transcode(src, template, dst):
    blob = open(src, "rb").read()
    tpl = open(template, "rb").read()

    if len(tpl) < HEADER_SIZE + 8:
        raise SystemExit("template too small")
    ver, hdr_size = struct.unpack_from("<II", tpl, 0)
    if ver != DEMO_VERSION or hdr_size != HEADER_SIZE:
        raise SystemExit("template header is not version %d / size %d (got %d / %d)"
                         % (DEMO_VERSION, HEADER_SIZE, ver, hdr_size))
    header = tpl[:HEADER_SIZE]
    footer_size = struct.unpack_from("<I", tpl, len(tpl) - 8)[0]
    footer = bytearray(tpl[len(tpl) - 8 - footer_size:])

    # ---- THE FOOTER CARRIES THE MAP NAME, AND IT MUST MATCH THE CAPTURE -------
    # PROVEN 2026-08-09. CL_Demo_StopRecord writes the footer body from clc+342144,
    # and CL_Demo_Play_f loads the level with DB_LoadLevelXAssets(clc+342416).
    #   342416 - 342144 = 272
    # so the map name lives at footer_body + 272. Verified by reading it out of six
    # demos: dday -> 'mp_d_day', airshipdemo -> 'mp_airship', x0147 ->
    # 'mp_forest_01', x03ae -> 'mp_shipment_s2'.
    #
    # A cross-map template therefore makes the engine load the TEMPLATE's map while
    # the gamestate names the capture's map. Observed exactly that: a forest capture
    # wrapped in dday's footer loaded mp_d_day and then crashed looking for
    # mp_forest_01.d3dbsp.
    want = capture_mapname(blob)
    have = bytes(footer[272:272 + 64]).split(b"\x00")[0].decode("ascii", "replace")
    if want and want != have:
        enc = want.encode("ascii")
        # Overwrite in place and clear the remainder of the old name, nothing more.
        footer[272:272 + len(enc) + 1] = enc + b"\x00"
        for i in range(272 + len(enc) + 1, 272 + max(len(have) + 1, len(enc) + 1)):
            footer[i] = 0
        print("   map name : footer+272 '%s' -> '%s' (patched to match the capture)"
              % (have, want))
    elif want:
        print("   map name : footer+272 '%s' (matches the capture)" % have)
    else:
        print("   map name : footer+272 '%s' -- capture map UNKNOWN, left alone" % have)
    footer = bytes(footer)

    # ids: 2 = network_data (snapshot), 3 = predicted_data (archive),
    #      7 = gamestate_message. Capture order is preserved, so snapshots and
    #      archives stay interleaved exactly as they occurred live.
    records = [(i, p) for i, p in iter_tlvs(blob) if i in (2, 3, 7)]
    if not records:
        raise SystemExit("no network/predicted/gamestate records in %s" % src)
    # The gamestate must lead: CL_ParseServerMessage_Internal rejects svc_gamestate
    # once connstate >= 8, and rejects snapshots while connstate < 9. Python's sort
    # is stable, so everything else keeps its captured order.
    records.sort(key=lambda r: 0 if r[0] == 7 else 1)

    body = bytearray()
    msgs = arch = skipped = 0
    seq = 0
    for tlv_id, payload in records:
        if tlv_id == 3:
            out = to_archive(payload)
            if out is None:
                skipped += 1
                continue
            body += out                       # type byte is already inside
            arch += 1
            continue
        seq += 1
        out, real_seq = to_payload(payload, seq)
        if out is None:
            skipped += 1
            seq -= 1
            continue
        # The type-2 header's own sequence goes to clc+0x2013C. Keep it the same
        # real number as the message sequence, exactly as a live packet would.
        body += b"\x02"
        body += struct.pack("<I", real_seq & 0x7FFFFFFF)
        body += struct.pack("<I", len(out))
        body += out
        msgs += 1
    body += b"\x00"                            # end-of-stream terminator
    kept = msgs + arch

    with open(dst, "wb") as f:
        f.write(header)
        f.write(body)
        f.write(footer)

    print("%s -> %s" % (os.path.basename(src), os.path.basename(dst)))
    print("   template : %s (header %d B, footer %d B)"
          % (os.path.basename(template), len(header), len(footer)))
    print("   packets  : %d written (%d snapshot, %d archive), %d skipped"
          % (kept, msgs, arch, skipped))
    if msgs:
        print("   ratio    : %.1f archive : 1 snapshot   (shipped demos ~11:1 for "
              "local/bot, ~1:1 for a networked match)" % (arch / float(msgs)))
    print("   size     : %d bytes" % (len(header) + len(body) + len(footer)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("template")
    ap.add_argument("dst")
    a = ap.parse_args()
    return transcode(a.src, a.template, a.dst)


if __name__ == "__main__":
    sys.exit(main())
