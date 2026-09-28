"""RUNS INSIDE IDA (MWR-PC). Apply anchor-derived names with basis comments.

Every name carries a comment stating how it was derived.
Nothing is applied over an existing name.
"""
import json, os

RE_DIR = r"c:\Users\joshu\OneDrive\Documents\GitHub\S2MP-Mod\re"
OUT = os.path.join(RE_DIR, "mwrpc_applied.json")


def main():
    import ida_name, ida_funcs, idc

    rows = json.load(open(os.path.join(RE_DIR, "mwrpc_corroborated.json"),
                          encoding="utf-8"))

    applied, failed, skipped = [], [], []
    for r in rows:
        ea, name = r["pc"], r["name"]

        cur = ida_name.get_name(ea)
        if cur and not (cur.startswith("sub_") or cur.startswith("nullsub_")
                        or cur.startswith("j_")):
            skipped.append([hex(ea), cur, name])
            continue

        if not ida_name.set_name(ea, name, ida_name.SN_NOWARN | ida_name.SN_NOCHECK):
            failed.append([hex(ea), name])
            continue

        if r["neighbours"] == 0:
            corro = "not yet testable (no named neighbours at this coverage)"
        elif r["corrob_out"] or r["corrob_in"]:
            corro = f"CONFIRMED - shared callees {r['corrob_out']}, shared callers {r['corrob_in']}"
        else:
            corro = (f"none yet ({r['neighbours']} named neighbour(s), no overlap) "
                     f"- weaker, re-check as coverage rises")

        anchors = ", ".join(repr(a[:60]) for a in r["anchors"][:3])
        cmt = (
            f"{name}\n"
            f"BASIS: string-anchor port from MWR-PS4 (2-h1_mp.elf, port 9999) - "
            f"SAME GAME, real symbols.\n"
            f"  {r['n_anchors']} anchor(s), each referenced by exactly ONE function on "
            f"BOTH sides, all implying PS4 {r['ps4'] and hex(r['ps4'])}.\n"
            f"  anchors: {anchors}\n"
            f"  tier: {r['tier']}\n"
            f"  call-graph corroboration (independent of strings): {corro}\n"
            f"NOTE: cross-binary port - only the NAME transfers, addresses differ."
        )
        ida_funcs.set_func_cmt(ida_funcs.get_func(ea), cmt, 1)
        applied.append([hex(ea), name])

    json.dump({"applied": applied, "failed": failed, "skipped": skipped},
              open(OUT, "w", encoding="utf-8"))
    return {"applied": len(applied), "failed": len(failed),
            "skipped_existing": len(skipped),
            "failed_sample": failed[:10]}


main()
