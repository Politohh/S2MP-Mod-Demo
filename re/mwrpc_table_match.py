"""Match PC binding tables to PS4 binding tables, then entries within a pair.

Match TABLES to TABLES first (by shared
binding-name set), and only then match entries inside a corresponding pair. A
bare binding name matched across binaries is careless - 'IsEnabled', 'GetTime',
'show' live in many different tables.

Acceptance: name-set ratio >= 0.60 OR absolute shared >= 5 (engines split and
merge tables between versions, which collapses the ratio while leaving the
overlap unmistakable).
"""
import json, os, collections

RE_DIR = os.path.dirname(os.path.abspath(__file__))
L = lambda n: json.load(open(os.path.join(RE_DIR, n), encoding="utf-8"))

pc_tables = L("mwrpc_tables2.json")
ps4_tables = L("ps4_tables.json")
pcfuncs = {int(k, 16): v for k, v in L("mwrpc_funcs.json").items()}

pc_named = {ea: v[1] for ea, v in pcfuncs.items()
            if not (v[1].startswith("sub_") or v[1].startswith("nullsub_")
                    or v[1].startswith("j_"))}
taken = set(pc_named.values())

matches, proposals = [], {}
used_ps4_name = set()

for pt in pc_tables:
    pset = {p[0] for p in pt["pairs"]}
    best = None
    for qt in ps4_tables:
        qset = {p[0] for p in qt["pairs"]}
        shared = pset & qset
        if not shared:
            continue
        ratio = len(shared) / min(len(pset), len(qset))
        if ratio >= 0.60 or len(shared) >= 5:
            if best is None or len(shared) > best[1]:
                best = (qt, len(shared), ratio)
    if not best:
        continue
    qt, nshared, ratio = best
    matches.append({"pc": pt["base"], "ps4": hex(qt["base"]),
                    "pc_n": pt["count"], "ps4_n": qt["count"],
                    "shared": nshared, "ratio": round(ratio, 2)})

    ps4_by_name = {p[0]: p for p in qt["pairs"]}
    for bind, fea_hex in pt["pairs"]:
        q = ps4_by_name.get(bind)
        if not q:
            continue
        ps4name = q[2]
        if not ps4name or ps4name.startswith("sub_"):
            continue
        fea = int(fea_hex, 16)
        proposals.setdefault(fea, []).append((ps4name, bind, pt["base"],
                                              hex(qt["base"])))

# one name, one function + never overwrite
final, conflicts, already = {}, 0, 0
for fea, cands in proposals.items():
    names = {c[0] for c in cands}
    if len(names) != 1:
        conflicts += 1
        continue
    nm = names.pop()
    if fea in pc_named:
        already += 1
        continue
    if nm in taken or nm in used_ps4_name:
        conflicts += 1
        continue
    used_ps4_name.add(nm)
    final[fea] = {"name": nm, "binding": cands[0][1],
                  "pc_table": cands[0][2], "ps4_table": cands[0][3]}

# validation: agreement with names derived independently (strings / call graph)
agree = tot = 0
mism = []
for fea, cands in proposals.items():
    if fea not in pc_named:
        continue
    tot += 1
    if any(c[0] == pc_named[fea] for c in cands):
        agree += 1
    else:
        mism.append((pc_named[fea], cands[0][0]))

print(f"table pairs matched        : {len(matches)}")
print(f"entry proposals            : {len(proposals):,}")
print(f"  dropped (conflict/taken) : {conflicts}")
print(f"  already named            : {already}")
print(f"APPLICABLE                 : {len(final):,}")
print(f"\nVALIDATION vs independently-derived names: {agree}/{tot} agree "
      f"({100.0*agree/max(1,tot):.1f}%)")
for a, b in mism[:8]:
    print(f"    existing {a[:52]:52} table says {b[:52]}")

json.dump({str(k): v for k, v in final.items()},
          open(os.path.join(RE_DIR, "mwrpc_table_names.json"), "w",
               encoding="utf-8"))

print("\nmatched tables:")
for m in sorted(matches, key=lambda m: -m["shared"])[:12]:
    print(f"  PC {m['pc']} ({m['pc_n']:3}) <-> PS4 {m['ps4']} ({m['ps4_n']:3})  "
          f"shared={m['shared']:3} ratio={m['ratio']}")
