"""Score CoD4-Mac -> CoD4X string-anchor matches into name proposals.

Matching rules:
  * anchor unambiguous on BOTH sides
  * unanimous vote per target function
  * one name, one function - a name claimed by 2+ targets is dropped, UNLESS one
    candidate's evidence is strictly stronger (see DOMINANCE below)
  * never overwrite an existing target name
  * distinctiveness, not length
  * function size is never used (cross-compiler; measured harmful on MWR)

DOMINANCE: collisions here have two shapes. Genuinely ambiguous ones - five
consecutive ClientCommand handlers, because the Mac build inlined what CoD4X
splits into siblings. And overwhelmingly one-sided ones - AimAssist_Init with 72
anchors against a neighbour with 1, because CoD4X split off a small helper that
happens to reference one of the same strings. Only the second shape is
recovered, and only at >= 3x the runner-up's anchor count with >= 3 anchors.
"""
import json, os, collections
from cod4x_common import RE_DIR, build_votes, is_named

DOM_RATIO = 3.0
DOM_MIN_ANCHORS = 3

votes, stats, ref, tgt = build_votes()
tgt_names = {int(k): v for k, v in tgt["names"].items()}

# ---- 1. unanimity + tiering ---------------------------------------------
cand, dropped = {}, collections.Counter()
for ea, vs in votes.items():
    if is_named(tgt_names.get(ea, "")):
        dropped["already_named"] += 1
        continue
    names = {n for _, n, _, _ in vs}
    if len(names) != 1:
        dropped["split_vote"] += 1
        continue
    name = names.pop()
    best = max(s for _, _, s, _ in vs)
    strong = any(s >= 2 for _, _, s, _ in vs)
    ref_ea = collections.Counter(r for _, _, _, r in vs).most_common(1)[0][0]
    tier = ("multi" if len(vs) >= 2 and strong else
            "single_strong" if best >= 3 else
            "single_weak")
    cand[ea] = {"name": name, "tier": tier, "n_anchors": len(vs),
                "best_score": best, "ref_ea": ref_ea,
                "anchors": sorted((a for a, _, _, _ in vs), key=len)[-4:]}

# ---- 2. one name, one function (with strict dominance recovery) ----------
byname = collections.defaultdict(list)
for ea, p in cand.items():
    byname[p["name"]].append(ea)

proposals = {}
recovered = ambiguous = 0
for name, eas in byname.items():
    if len(eas) == 1:
        proposals[eas[0]] = cand[eas[0]]
        continue
    ranked = sorted(eas, key=lambda e: (-cand[e]["n_anchors"],
                                        -cand[e]["best_score"]))
    top, runner = cand[ranked[0]], cand[ranked[1]]
    if (top["n_anchors"] >= DOM_MIN_ANCHORS and top["best_score"] >= 2 and
            top["n_anchors"] >= DOM_RATIO * runner["n_anchors"]):
        p = dict(top)
        p["tier"] = "dominant"
        p["rivals"] = len(eas) - 1
        p["dominance"] = f"{top['n_anchors']} anchors vs {runner['n_anchors']}"
        proposals[ranked[0]] = p
        recovered += 1
    else:
        ambiguous += 1

existing = {n for n in tgt_names.values() if is_named(n)}
clash = [ea for ea, p in proposals.items() if p["name"] in existing]
for ea in clash:
    proposals.pop(ea)

print(f"reference strings with an owner : {len(ref['owners']):,}")
print(f"  also present in target        : {stats['shared']:,}")
print(f"  dropped, 2+ owners in ref     : {stats['ambiguous_ref']:,}")
print(f"  dropped, 2+ owners in target  : {stats['ambiguous_target']:,}")
print(f"  dropped, ref name unusable    : {stats['ref_name_rejected']:,}")
print(f"  USABLE ANCHORS                : {stats['anchor_usable']:,}")
print(f"target funcs receiving votes    : {len(votes):,}")
print(f"  dropped, already named        : {dropped['already_named']}")
print(f"  dropped, split vote           : {dropped['split_vote']}")
print(f"  collisions recovered by dominance : {recovered}")
print(f"  collisions left ambiguous         : {ambiguous}")
print(f"  dropped, name already in use  : {len(clash)}")
tiers = collections.Counter(p["tier"] for p in proposals.values())
print(f"APPLICABLE                      : {len(proposals):,}")
for t in ("multi", "dominant", "single_strong", "single_weak"):
    print(f"     {t:14}: {tiers.get(t,0):,}")

json.dump({str(k): v for k, v in proposals.items()},
          open(os.path.join(RE_DIR, "cod4x_proposals.json"), "w", encoding="utf-8"))

print("\nsample (dominant - collision winners):")
for ea, p in [(k, v) for k, v in proposals.items() if v["tier"] == "dominant"][:12]:
    print(f"  0x{ea:<8x} {p['name'][:44]:44} {p['dominance']}")
print("\nsample (single_weak - the risky tier):")
for ea, p in [(k, v) for k, v in proposals.items() if v["tier"] == "single_weak"][:8]:
    print(f"  0x{ea:<8x} {p['name'][:40]:40} <- {p['anchors'][-1][:44]!r}")
