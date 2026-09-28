"""Score MWR-PS4 -> MWR-PC anchor matches into applicable name proposals.

Matching rules:
  * anchor must be unambiguous on BOTH sides (1 PS4 owner by construction,
    1 PC owner here)
  * all anchors backing a PC function must agree on the PS4 name (unanimous)
  * one name, one function - a PS4 name claimed by 2+ PC functions is dropped
  * never overwrite an existing PC name
  * distinctiveness, not length
"""
import json, re, os, collections

RE_DIR = os.path.dirname(os.path.abspath(__file__))
d = json.load(open(os.path.join(RE_DIR, "mwrpc_anchor_hits.json"), encoding="utf-8"))
hits = d["hits"]
already = {int(k): v for k, v in d["pc_already_named"].items()}

FMT = re.compile(r"%[-+ #0-9.*]*[hlLqjzt]*[diouxXeEfgGaAcspn%]")
NOISE = {"error", "warning", "failed", "invalid", "unknown", "true", "false",
         "none", "null", "yes", "no", "on", "off", "default", "test", "temp"}


def distinctive(s):
    """Does an identifier-ish token survive after stripping format specifiers?"""
    core = FMT.sub(" ", s)
    toks = re.findall(r"[A-Za-z_][A-Za-z0-9_]{2,}", core)
    toks = [t for t in toks if t.lower() not in NOISE]
    if not toks:
        return 0
    score = 0
    for t in toks:
        if "_" in t or re.search(r"[a-z][A-Z]", t):   # snake or camel
            score += 2
        elif len(t) >= 6:
            score += 1
    if len(core.strip()) >= 24:
        score += 1
    return score


# ---- 1. keep anchors with exactly one PC owner ---------------------------
votes = collections.defaultdict(list)      # pc_ea -> [(anchor, ps4_name, score)]
ambiguous_pc = 0
for anchor, h in hits.items():
    if len(h["pc"]) != 1:
        ambiguous_pc += 1
        continue
    pc = h["pc"][0]
    votes[pc].append((anchor, h["ps4_name"], distinctive(anchor)))

# ---- 2. unanimity + tiering ---------------------------------------------
proposals, split, skipped_named = {}, 0, 0
for pc, vs in votes.items():
    if pc in already:
        skipped_named += 1
        continue
    names = {n for _, n, _ in vs}
    if len(names) != 1:
        split += 1
        continue
    name = names.pop()
    best = max(s for _, _, s in vs)
    strong = [a for a, _, s in vs if s >= 2]
    tier = ("multi" if len(vs) >= 2 and strong else
            "single_strong" if best >= 3 else
            "single_weak")
    proposals[pc] = {"name": name, "tier": tier, "n_anchors": len(vs),
                     "best_score": best,
                     "anchors": [a for a, _, _ in vs][:4]}

# ---- 3. one name, one function ------------------------------------------
byname = collections.defaultdict(list)
for pc, p in proposals.items():
    byname[p["name"]].append(pc)
collisions = {n: eas for n, eas in byname.items() if len(eas) > 1}
for n, eas in collisions.items():
    for pc in eas:
        proposals.pop(pc, None)

# never reuse a name already worn by an existing PC function
existing_names = set(already.values())
clash_existing = [pc for pc, p in proposals.items() if p["name"] in existing_names]
for pc in clash_existing:
    proposals.pop(pc)

tiers = collections.Counter(p["tier"] for p in proposals.values())
print(f"anchors with a PC xref        : {len(hits):,}")
print(f"  dropped, 2+ PC owners       : {ambiguous_pc:,}")
print(f"  PC funcs receiving votes    : {len(votes):,}")
print(f"  dropped, already named      : {skipped_named}")
print(f"  dropped, split vote         : {split}")
print(f"  dropped, name collision     : {len(collisions)} names / "
      f"{sum(len(v) for v in collisions.values())} funcs")
print(f"  dropped, name already in use: {len(clash_existing)}")
print(f"APPLICABLE                    : {len(proposals):,}")
for t in ("multi", "single_strong", "single_weak"):
    print(f"     {t:14}: {tiers.get(t,0):,}")

json.dump({str(k): v for k, v in proposals.items()},
          open(os.path.join(RE_DIR, "mwrpc_proposals.json"), "w", encoding="utf-8"))

print("\nsample (multi-anchor):")
for pc, p in list((k, v) for k, v in proposals.items() if v["tier"] == "multi")[:12]:
    print(f"  0x{pc:<8x} {p['name'][:58]:58} n={p['n_anchors']}")
print("\nsample (single_weak - these are the risky tier):")
for pc, p in list((k, v) for k, v in proposals.items() if v["tier"] == "single_weak")[:8]:
    print(f"  0x{pc:<8x} {p['name'][:44]:44} <- {p['anchors'][0][:52]!r}")
