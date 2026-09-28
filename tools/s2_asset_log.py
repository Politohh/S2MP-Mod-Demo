#!/usr/bin/env python3
"""
s2_asset_log.py -- OFFLINE accounting of S2's asset-pool behaviour.

No game, no probe. Reads artefacts the mod already wrote:

  * main/s2mp_console.log            the [assets] brackets emitted by demo_native.cpp
  * asset_census_image.<tag>.txt     full image-asset name dumps

and reconstructs the whole timeline: what each level load registered, what (if
anything) was released, how the loaded-zone set evolved, how much headroom was
left, and the exact point the 40192 'image' pool overflowed.

WHY THIS EXISTS
    The image limit was previously chased with live runs. Everything needed to
    account for it is already in these two artefacts, so the analysis belongs
    offline rather than requiring another live run.

USAGE
    python tools/s2_asset_log.py <console.log> [more.log ...]
    python tools/s2_asset_log.py --diff BASE.txt OTHER.txt
    python tools/s2_asset_log.py <console.log> --diff BASE.txt OTHER.txt

FACTS BAKED IN (measured)
    image asset type = 21, pool = 40192 (also the per-tier stride of the
    residency bitmap at IDA 0x79DB100).
"""

import argparse
import os
import re
import sys
from collections import Counter

IMAGE_TYPE = 21
IMAGE_POOL = 40192

# ---------------------------------------------------------------- log parsing

RE_CENSUS = re.compile(r"^\[assets\] ===== census: (.+?) =====")
RE_HEADLINE = re.compile(
    r"^\[assets\] IMAGE (\d+)/(\d+) \([\d.]+%\), (\d+) registered since '([^']*)'; "
    r"lifetime registrations (\d+), so only (-?\d+) have ever been freed"
)
RE_ENTER = re.compile(
    r"^\[assets\] DB_LoadLevelXAssets\('([^']*)', (-?\d+)\) ENTER: (\d+) zones, "
    r"(\d+) image registrations so far(.*)$"
)
RE_LEAVE = re.compile(
    r"^\[assets\] DB_LoadLevelXAssets\('([^']*)'\) LEAVE: zones (\d+) -> (\d+), "
    r"image registrations (\d+) -> (\d+) \(\+(\d+)\)"
)
RE_ZONES = re.compile(r"^\[assets\] zones (.+?):(.*)$")
RE_ZONECOUNT = re.compile(r"^\[assets\] (\d+) zone\(s\) loaded (.+)$")
RE_GATE = re.compile(
    r"^\[assets\] unload gate sub_38F590\(\)=(\d) -> mask (\d+) \(0x[0-9A-Fa-f]+\)"
)
RE_LIMIT = re.compile(
    r"^\[assets\] ASSET LIMIT: type (\d+) '([^']*)' pool=(\d+)"
)
RE_ROW = re.compile(
    r"^\[assets\]\s+(\d+) (\S+)\s+(\d+)/(\d+)\s+[\d.]+%\s+(\d+)\s+(\d+)"
)
RE_PLAY = re.compile(r"^\[native\] cl_demo_play (\S+)")
RE_ZONE_FF = re.compile(r"^Loading Zone: (\S+)\.ff")
RE_MAPLOAD = re.compile(r"^Loading Map: (\S+)")

ZONE_ENTRY = re.compile(r"(\S+)\((\w+)\)")


class Event:
    def __init__(self, kind, line_no):
        self.kind = kind
        self.line_no = line_no
        self.data = {}

    def __repr__(self):
        return "<%s@%d %r>" % (self.kind, self.line_no, self.data)


def parse_log(path):
    """Return a list of Events in file order."""
    events = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for n, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")

            m = RE_PLAY.match(line)
            if m:
                e = Event("play", n)
                e.data["demo"] = m.group(1)
                events.append(e)
                continue

            m = RE_CENSUS.match(line)
            if m:
                e = Event("census", n)
                e.data["why"] = m.group(1)
                e.data["rows"] = {}
                events.append(e)
                continue

            m = RE_HEADLINE.match(line)
            if m and events and events[-1].kind == "census":
                e = events[-1]
                e.data.update(
                    live=int(m.group(1)),
                    pool=int(m.group(2)),
                    since=int(m.group(3)),
                    mark=m.group(4),
                    lifetime=int(m.group(5)),
                    freed=int(m.group(6)),
                )
                continue

            m = RE_ROW.match(line)
            if m and events and events[-1].kind == "census":
                events[-1].data["rows"][int(m.group(1))] = dict(
                    name=m.group(2),
                    live=int(m.group(3)),
                    pool=int(m.group(4)),
                    calls=int(m.group(5)),
                    since=int(m.group(6)),
                )
                continue

            m = RE_ENTER.match(line)
            if m:
                e = Event("load_enter", n)
                e.data.update(
                    map=m.group(1),
                    flags=int(m.group(2)),
                    zones=int(m.group(3)),
                    img=int(m.group(4)),
                    early="EARLY RETURN" in m.group(5),
                )
                events.append(e)
                continue

            m = RE_LEAVE.match(line)
            if m:
                e = Event("load_leave", n)
                e.data.update(
                    map=m.group(1),
                    zones_before=int(m.group(2)),
                    zones_after=int(m.group(3)),
                    img_before=int(m.group(4)),
                    img_after=int(m.group(5)),
                    added=int(m.group(6)),
                )
                events.append(e)
                continue

            m = RE_ZONES.match(line)
            if m:
                zones = ZONE_ENTRY.findall(m.group(2))
                if events and events[-1].kind == "zones" and events[-1].data["why"] == m.group(1):
                    events[-1].data["zones"].extend(zones)   # wrapped continuation line
                else:
                    e = Event("zones", n)
                    e.data["why"] = m.group(1)
                    e.data["zones"] = list(zones)
                    events.append(e)
                continue

            m = RE_GATE.match(line)
            if m:
                e = Event("gate", n)
                e.data.update(gate=int(m.group(1)), mask=int(m.group(2)))
                events.append(e)
                continue

            m = RE_LIMIT.match(line)
            if m:
                e = Event("limit", n)
                e.data.update(type=int(m.group(1)), name=m.group(2), pool=int(m.group(3)))
                events.append(e)
                continue

            m = RE_MAPLOAD.match(line)
            if m:
                e = Event("maploaded", n)
                e.data["map"] = m.group(1)
                events.append(e)
                continue
    return events


# ------------------------------------------------------------------- analysis

def zone_flag(v):
    try:
        return int(v, 16)
    except ValueError:
        return 0


def report(path, events):
    print("=" * 78)
    print("LOG  %s" % path)
    print("=" * 78)

    censuses = [e for e in events if e.kind == "census" and "live" in e.data]
    if not censuses:
        print("  no [assets] census lines in this log -- nothing to account for.")
        return

    baseline = censuses[0].data["live"]
    print()
    print("IMAGE POOL ACCOUNTING   (type %d, pool %d)" % (IMAGE_TYPE, IMAGE_POOL))
    print("-" * 78)
    print("  %-42s %7s %7s %7s" % ("event", "live", "headrm", "freed"))
    for e in censuses:
        d = e.data
        print("  %-42s %7d %7d %7d"
              % (d["why"][:42], d["live"], d["pool"] - d["live"], -d["freed"]))

    peak = max(c.data["live"] for c in censuses)
    print()
    print("  cold-menu baseline .............. %6d images (%.1f%% of the pool)"
          % (baseline, 100.0 * baseline / IMAGE_POOL))
    print("  headroom above the baseline ..... %6d images" % (IMAGE_POOL - baseline))
    print("  peak observed ................... %6d images" % peak)

    # "freed" is reported as lifetime-registrations minus live, so a NEGATIVE
    # value means live exceeds our call count (assets registered before the hook
    # installed). What matters is that it never grows: nothing is being released.
    freed = [c.data["freed"] for c in censuses]
    span = max(freed) - min(freed)
    print("  releases observed all session ... %6d images   <-- %s"
          % (span, "NOTHING IS EVER RELEASED" if span < 1000 else "some release happened"))

    # ---- per level load
    print()
    print("LEVEL LOADS")
    print("-" * 78)
    enters = [e for e in events if e.kind == "load_enter"]
    leaves = [e for e in events if e.kind == "load_leave"]
    for i, en in enumerate(enters):
        lv = leaves[i] if i < len(leaves) else None
        tag = "  EARLY RETURN (skips unload AND load)" if en.data["early"] else ""
        print("  #%d  %-16s zones %2d%s%s"
              % (i + 1, en.data["map"], en.data["zones"],
                 " -> %d" % lv.data["zones_after"] if lv else "", tag))
        if lv:
            zdelta = lv.data["zones_after"] - lv.data["zones_before"]
            print("      zones %+d (%s), images +%d during the call"
                  % (zdelta,
                     "PURE ADDITION - nothing released" if zdelta >= 0 else "released some",
                     lv.data["added"]))

    gates = [e for e in events if e.kind == "gate"]
    if gates:
        print()
        print("UNLOAD GATE  (DB_LoadLevelXAssets' own release mask)")
        print("-" * 78)
        for g in gates:
            mask = g.data["mask"]
            print("  sub_38F590()=%d -> mask %d (0x%X)  %s"
                  % (g.data["gate"], mask, mask,
                     "covers NOTHING loaded -> release is a no-op" if mask == 512
                     else "covers the level zones"))

    # ---- zone accounting: what accumulated
    zlists = [e for e in events if e.kind == "zones"]
    if zlists:
        first = {n: zone_flag(f) for n, f in zlists[0].data["zones"]}
        last = {n: zone_flag(f) for n, f in zlists[-1].data["zones"]}
        gained = [n for n in last if n not in first]
        lost = [n for n in first if n not in last]
        print()
        print("ZONES  (%s  ->  %s)" % (zlists[0].data["why"], zlists[-1].data["why"]))
        print("-" * 78)
        print("  start %d zones, end %d zones" % (len(first), len(last)))
        print("  ACCUMULATED (%d): %s" % (len(gained), " ".join(sorted(gained)) or "-"))
        print("  RELEASED    (%d): %s" % (len(lost), " ".join(sorted(lost)) or "-"))

        # group by flag so the mask arithmetic is checkable by eye
        byflag = {}
        for n, f in last.items():
            byflag.setdefault(f, []).append(n)
        print()
        print("  loaded zones by flag word:")
        for f in sorted(byflag):
            print("    0x%-3X (%2d) %s" % (f, len(byflag[f]), " ".join(sorted(byflag[f]))))

        print()
        print("  what each release mask in the engine WOULD free from this set:")
        for label, mask in (("DB_LoadLevelXAssets, gate true ", 396),
                            ("DB_LoadLevelXAssets, gate false", 512),
                            ("sub_837E0  (map change)        ", 136),
                            ("sub_48C0C0 (live map spawn)    ", 388),
                            ("sub_48C0C0 (| 0x200 variant)   ", 388 | 0x200),
                            ("sub_8DE90  (back to frontend)  ", 0x3BC)):
            hit = sorted(n for n, f in last.items() if f & mask)
            print("    mask %-5d 0x%-4X %-2d zone(s)  %s"
                  % (mask, mask, len(hit), " ".join(hit) if hit else "<nothing>"))

    lim = [e for e in events if e.kind == "limit"]
    if lim:
        d = lim[0].data
        print()
        print("OVERFLOW")
        print("-" * 78)
        print("  DB_RaiseAssetLimitError: type %d '%s' pool %d"
              % (d["type"], d["name"], d["pool"]))
        print("  baseline %d + accumulated level content exceeded %d."
              % (baseline, IMAGE_POOL))


# --------------------------------------------------------------- census diff

def load_census(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return [l.strip() for l in fh if l.strip()]


def bucket(name):
    """Coarse family for a generated image name, for a readable breakdown."""
    n = name
    if n.startswith("~"):
        # generated/compiled variants: ~&-c<name>, ~&-n<name>, ~$...&<name>
        i = n.find("-")
        j = n.find("&")
        cut = max(i, j)
        n = n[cut + 1:] if cut >= 0 else n[1:]
        if n[:1] in "cn":
            n = n[1:]
    parts = n.split("_")
    return "_".join(parts[:2]) if len(parts) >= 2 else (parts[0] if parts else "?")


def diff_census(base_path, other_path, top=25):
    base = set(load_census(base_path))
    other = set(load_census(other_path))
    added = other - base
    removed = base - other

    print()
    print("=" * 78)
    print("CENSUS DIFF")
    print("  BASE  %-45s %6d images" % (os.path.basename(base_path), len(base)))
    print("  OTHER %-45s %6d images" % (os.path.basename(other_path), len(other)))
    print("=" * 78)
    print("  ADDED   %6d" % len(added))
    print("  REMOVED %6d   %s" % (len(removed),
                                  "<-- nothing was released" if not removed else ""))
    print("  NET     %+6d" % (len(other) - len(base)))

    for label, s in (("ADDED", added), ("REMOVED", removed)):
        if not s:
            continue
        c = Counter(bucket(n) for n in s)
        print()
        print("  %s, by name family (top %d of %d families):" % (label, top, len(c)))
        for fam, cnt in c.most_common(top):
            print("    %6d  %s" % (cnt, fam))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*", help="console log(s) to account for")
    ap.add_argument("--diff", nargs=2, metavar=("BASE", "OTHER"),
                    help="diff two asset_census_image.*.txt dumps")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    if not args.logs and not args.diff:
        ap.print_help()
        return 1

    for path in args.logs:
        if not os.path.exists(path):
            print("missing: %s" % path, file=sys.stderr)
            continue
        report(path, parse_log(path))

    if args.diff:
        diff_census(args.diff[0], args.diff[1], args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
