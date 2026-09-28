#!/usr/bin/env python3
"""
s2_keyframe_sim.py -- OFFLINE simulation of S2's demo keyframe ring and rewind.

WHY THIS EXISTS
    Rewind was reported as "works very rarely, glitches out". The chain is:
        sub_914790            writes a keyframe payload into a 2 MB ring + a slot
        usable_slots()        (OURS) picks which slots are valid seek targets
        ProcessKeyFrameJump   reads the payload back and replays messages
    All three are pure bookkeeping over a ring buffer and a 250-entry slot table,
    so they can be simulated exactly, with no game.

MODEL SOURCE -- every constant below is read off the S2 decompile, not guessed.

  sub_914790 (keyframe writer), tail:
      v43 = (*(DWORD*)(pd + 2188584) + 1) % 250;      # ADVANCE the index FIRST
      *(DWORD*)(pd + 2188584) = v43;                  # so widx == NEWEST slot
      zero slot v43 (three _OWORDs == 48 bytes)
      slot[+0]  = v7                                  # ring cursor at START
      slot[+4]  = *(DWORD*)(pd + 24)                  # demo FILE offset
      slot[+24] = cmdSeq
      slot[+28] = (end - start) % 0x1FFFFF            # LENGTH
      slot[+32] = clc[+346128]                        # replay range START
      slot[+36] = clientActive[+90472]                # replay range END
      slot[+8]  = clientActive[+25380]                # time (cl.snap.serverTime)

  ring writes wrap at 0x1FFFFF (NOT 0x200000 -- the writer splits with
      v14 = 0x1FFFFF - cursor  then sets cursor = 0), payload base pd + 79432.

  sub_914790 ALSO invalidates every slot whose [start, start+len) overlaps the
  region just written, by zeroing the whole 48-byte slot (v51 is _OWORD*, and
  `*v51 = 0; v51[1] = 0; v51[2] = 0` clears 48 bytes, with v51 += 3 per step).

  ProcessKeyFrameJump restores the world with
      for (j = slot[+32]; j != slot[+36]; j = (j + 1) % 256)
          CL_Demo_ReadDemoMessage(...)
  reading through the MEMORY reader seeded at slot[+0]. So a jump is only
  meaningful when the payload is intact AND slot[+32] != slot[+36].

USAGE
    python tools/s2_keyframe_sim.py                # validate + full sweep
    python tools/s2_keyframe_sim.py --validate     # only check vs the real log
    python tools/s2_keyframe_sim.py --verbose
"""

import argparse
import random
import sys

RING_BYTES = 0x200000        # 2 MiB payload buffer at PlaybackData + 79432
RING_MOD = 0x1FFFFF          # the writer's actual wrap point -- note != RING_BYTES
NUM_SLOTS = 250
MSG_RING = 256               # the replay message ring, (j + 1) % 256

SLOT_FIELDS = ("mem_off", "file_off", "time", "cmd_seq", "length", "abs_start",
               "replay_a", "replay_b", "true_msgs")


class Slot(dict):
    """A 48-byte keyframe slot. Zeroed == never written / invalidated."""

    def __init__(self):
        super().__init__({k: 0 for k in SLOT_FIELDS})

    @property
    def written(self):
        return self["length"] > 0


class KeyframeRing:
    """Faithful model of sub_914790's ring + slot bookkeeping."""

    def __init__(self):
        self.slots = [Slot() for _ in range(NUM_SLOTS)]
        self.widx = 0            # PlaybackData + 2188584
        self.cursor = 0          # PlaybackData + 2188588
        # Ground truth the engine does not keep: total bytes ever written. A
        # payload starting at ABSOLUTE position P is intact iff it still lies
        # within the last RING_MOD bytes -- O(1), and exact, because the ring
        # is written strictly sequentially.
        self.total = 0
        self.invalidated = 0

    def _advance(self, n):
        """Writer's wrap: split at 0x1FFFFF, then restart at 0."""
        start = self.cursor
        if start + n >= RING_BYTES:
            head = RING_MOD - start
            if head > 0:
                n -= head
            self.cursor = n
        else:
            self.cursor = start + n
        return start

    def write_keyframe(self, length, time_ms, file_off, cmd_seq,
                       replay_a, replay_b, true_msgs=0):
        start = self._advance(length)
        abs_start = self.total
        self.total += length

        # advance-then-write, exactly as the engine does
        self.widx = (self.widx + 1) % NUM_SLOTS
        self.slots[self.widx] = Slot()

        # The engine's own invalidation: sub_914790 zeroes every slot whose
        # region the write just passed over. Cumulatively that is exactly
        # "anything older than the last RING_MOD bytes".
        for i, s in enumerate(self.slots):
            if i == self.widx or not s.written:
                continue
            if s["abs_start"] + s["length"] > self.total - RING_MOD:
                continue
            self.slots[i] = Slot()
            self.invalidated += 1

        s = self.slots[self.widx]
        s.update(mem_off=start, file_off=file_off, time=time_ms,
                 cmd_seq=cmd_seq, length=length,
                 replay_a=replay_a, replay_b=replay_b)
        s["abs_start"] = abs_start
        s["true_msgs"] = true_msgs
        return self.widx

    def payload_intact(self, idx):
        """GROUND TRUTH: does this slot's payload still hold its own bytes?"""
        s = self.slots[idx]
        if not s.written:
            return False
        return s["abs_start"] >= self.total - RING_MOD

    def jump_would_restore(self, idx):
        """What ProcessKeyFrameJump actually achieves for this slot."""
        s = self.slots[idx]
        if not self.payload_intact(idx):
            return "CORRUPT"          # reads another keyframe's bytes
        if s["replay_a"] == s["replay_b"]:
            return "NO_SNAPSHOT"      # replay loop runs zero times
        # ⭐ THE 256-RING WRAP. The replay loop counts with
        #     for (j = a; j != b; j = (j + 1) % 256)
        # so it performs ((b - a) mod 256) iterations. If MORE than 256 messages
        # were actually written between this keyframe and the next, the true
        # count aliases: (true mod 256) messages are replayed instead of `true`,
        # and the payload is read with the WRONG message count. Nothing in the
        # slot records the true figure, so neither the engine nor we can detect
        # it after the fact -- it has to be prevented by keyframing often enough.
        if s["true_msgs"] > MSG_RING - 1:
            return "RING_WRAPPED"
        if s["true_msgs"] != (s["replay_b"] - s["replay_a"]) % MSG_RING:
            return "RING_WRAPPED"
        return "OK"


def usable_slots(ring, variant="current"):
    """
    THE CODE UNDER TEST -- demo_native.cpp usable_slots(), transcribed.

      "original"  what shipped in 7C811B97: no empty-replay filter at all.
                  This is the POSITIVE CONTROL -- the sim must catch this, or it
                  is not testing anything.
      "buggy"     empty-replay `continue` placed BEFORE `acc += length`, so a
                  rejected slot's bytes go uncounted.
      "current"   56CAAC56: count the bytes, THEN filter.
    """
    out, acc = [], 0
    for k in range(NUM_SLOTS):
        i = (ring.widx - k) % NUM_SLOTS
        s = ring.slots[i]
        if s["length"] <= 0 or s["time"] < 0:
            continue
        empty = s["replay_a"] == s["replay_b"]
        if variant == "original":
            acc += s["length"]
            if acc > RING_BYTES:
                break
        elif variant == "buggy":
            if empty:
                continue
            acc += s["length"]
            if acc > RING_BYTES:
                break
        else:
            acc += s["length"]
            if acc > RING_BYTES:
                break
            if empty:
                continue
        out.append(i)
    return out


def pick_for_target(ring, usable, target_ms):
    """demo_seek's choice: nearest usable keyframe at or before the target."""
    pick, pick_t = -1, -1
    for i in usable:
        t = ring.slots[i]["time"]
        if t <= target_ms and t > pick_t:
            pick, pick_t = i, t
    if pick < 0:
        for i in usable:
            t = ring.slots[i]["time"]
            if pick_t < 0 or t < pick_t:
                pick, pick_t = i, t
    return pick


# ---------------------------------------------------------------------------
#  1. VALIDATE the model against REAL logged values before trusting it
# ---------------------------------------------------------------------------
REAL_LOG = [
    # slot, time,  file_off, length, cmd_seq, mem_off, replay
    (1, 4350, 81439, 88616, 21, 88931, (0, 0)),
    (2, 14350, 496789, 116730, 42, 177547, (0, 153)),
    (3, 24350, 930232, 89051, 57, 294277, (153, 156)),
]


def validate():
    print("=== validating the ring model against the user's real log ===")
    ok = True
    for a, b in zip(REAL_LOG, REAL_LOG[1:]):
        predicted = a[5] + a[3]          # mem_off + length
        actual = b[5]
        good = predicted == actual
        ok &= good
        print("  slot %d mem_off %7d + len %6d = %7d   slot %d mem_off = %7d   %s"
              % (a[0], a[5], a[3], predicted, b[0], actual,
                 "MATCH" if good else "MISMATCH"))
    print("  -> ring is contiguous and the writer model is %s\n"
          % ("CONFIRMED" if ok else "WRONG"))

    print("  replay ranges seen live:")
    for slot, t, _, _, _, _, rp in REAL_LOG:
        print("    slot %d t=%-6d replay=[%d..%d] %s" %
              (slot, t, rp[0], rp[1],
               "EMPTY -> restores NO snapshot" if rp[0] == rp[1] else "ok"))
    print()
    return ok


# ---------------------------------------------------------------------------
#  2. SWEEP: play demos of many shapes, seek constantly, count real failures
# ---------------------------------------------------------------------------
def run_session(seed, kf_bytes_range, n_keyframes, empty_first=True,
                variant="current", verbose=False, msgs_per_kf=(1, 8)):
    rnd = random.Random(seed)
    ring = KeyframeRing()
    msg_idx = 0
    stats = dict(offered=0, corrupt=0, no_snapshot=0, ok=0, no_target=0,
                 ring_wrapped=0)

    for n in range(n_keyframes):
        length = rnd.randint(*kf_bytes_range)
        t = 4350 + n * rnd.randint(800, 5000)
        # first keyframe of a session has nothing buffered -> empty replay
        if n == 0 and empty_first:
            a = b = msg_idx
            true_msgs = 0
        else:
            a = msg_idx
            true_msgs = rnd.randint(*msgs_per_kf)
            msg_idx = (msg_idx + true_msgs) % MSG_RING
            b = msg_idx
        ring.write_keyframe(length, t, 80000 + n * 400000, 20 + n, a, b,
                            true_msgs)

        # after each keyframe, try to seek somewhere back
        usable = usable_slots(ring, variant)
        if not usable:
            stats["no_target"] += 1
            continue
        for _ in range(3):
            target = rnd.randint(0, max(1, t))
            pick = pick_for_target(ring, usable, target)
            if pick < 0:
                stats["no_target"] += 1
                continue
            stats["offered"] += 1
            verdict = ring.jump_would_restore(pick)
            if verdict == "OK":
                stats["ok"] += 1
            elif verdict == "RING_WRAPPED":
                stats["ring_wrapped"] += 1
                if verbose:
                    print("    RING_WRAPPED: slot %d (t=%d) true=%d msgs but the "
                          "256-ring encodes %d"
                          % (pick, ring.slots[pick]["time"],
                             ring.slots[pick]["true_msgs"],
                             (ring.slots[pick]["replay_b"]
                              - ring.slots[pick]["replay_a"]) % MSG_RING))
            elif verdict == "CORRUPT":
                stats["corrupt"] += 1
                if verbose:
                    print("    CORRUPT: slot %d (t=%d) payload overwritten"
                          % (pick, ring.slots[pick]["time"]))
            else:
                stats["no_snapshot"] += 1
                if verbose:
                    print("    NO_SNAPSHOT: slot %d (t=%d) replay=[%d..%d]"
                          % (pick, ring.slots[pick]["time"],
                             ring.slots[pick]["replay_a"],
                             ring.slots[pick]["replay_b"]))
    stats["invalidated"] = ring.invalidated
    return stats


def sweep(label, variant, verbose=False):
    print("=== %s ===" % label)
    total = dict(offered=0, corrupt=0, no_snapshot=0, ok=0, no_target=0,
                 ring_wrapped=0)
    # payload sizes bracket the ~88-117 KB measured live; the small case forces
    # many keyframes into the ring, the large case forces rapid wrapping.
    # (payload range, keyframe count, messages between keyframes)
    # The last two shapes model a SPARSE keyframe cadence -- the case where more
    # than 256 demo messages pass between keyframes.
    shapes = [((88000, 118000), 40, (1, 8)), ((88000, 118000), 120, (1, 8)),
              ((200000, 400000), 40, (1, 8)), ((20000, 40000), 200, (1, 8)),
              ((88000, 118000), 60, (120, 400)), ((88000, 118000), 60, (200, 900))]
    for si, (rng, n, mpk) in enumerate(shapes):
        for seed in range(25):
            s = run_session(seed * 97 + si, rng, n, variant=variant,
                            verbose=verbose and seed == 0, msgs_per_kf=mpk)
            for k in total:
                total[k] += s[k]
    bad = total["corrupt"] + total["no_snapshot"] + total["ring_wrapped"]
    print("  seek targets offered : %d" % total["offered"])
    print("    would RESTORE ok   : %d" % total["ok"])
    print("    CORRUPT payload    : %d" % total["corrupt"])
    print("    NO snapshot        : %d" % total["no_snapshot"])
    print("    256-ring WRAPPED   : %d" % total["ring_wrapped"])
    print("  no target available  : %d  (honest refusal, not a failure)"
          % total["no_target"])
    print("  -> %s\n" % ("ALL OFFERED TARGETS ARE VALID" if bad == 0
                         else "!!! %d BAD TARGETS OFFERED !!!" % bad))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--validate", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not validate():
        print("model does not match reality -- fix the model before trusting it")
        return 2
    if args.validate:
        return 0

    print("Each 'offered' target is one demo_seek would jump to. A target is BAD")
    print("if ProcessKeyFrameJump could not actually restore from it.\n")

    bad_ctrl = sweep("POSITIVE CONTROL -- build 7C811B97, no empty-replay filter",
                     "original", args.verbose)
    print("-" * 68)
    bad_buggy = sweep("filter BEFORE counting bytes (the ordering slip)",
                      "buggy", args.verbose)
    print("-" * 68)
    bad_now = sweep("CURRENT -- build 56CAAC56, count bytes THEN filter",
                    "current", args.verbose)

    print("=" * 68)
    if bad_ctrl == 0:
        print("INCONCLUSIVE: the positive control did NOT fail, so this sim is")
        print("not exercising the defect and its PASS means nothing. Fix the")
        print("harness before trusting any result below.")
        return 2
    print("Positive control FAILED as designed (%d bad targets) -- the sim has"
          % bad_ctrl)
    print("teeth, so a PASS below is meaningful.\n")
    if bad_now == 0:
        print("PASS: current logic offers only restorable keyframes.")
    else:
        print("FAIL: current logic offers %d unrestorable targets." % bad_now)
    print("\nordering slip: %d bad targets (vs %d current) -- %s"
          % (bad_buggy, bad_now,
             "load-bearing" if bad_buggy > bad_now else
             "no effect HERE, because the engine invalidates overwritten slots "
             "itself; kept correct anyway"))
    return 0 if bad_now == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
