"""Score ft8_piece_harness output against jt9 (the <wav>.ws321d3.txt caches).

  python tools/wsjtx_port/score_pieces.py OUT.txt [OUT2.txt ...]

Per run: BASE (ft8_lib's decode) and PIECE (WSJT-X ft8b on the same
candidates), each split into messages jt9 also has (confirmed) and messages
it does not (not confirmed - false or beyond jt9; counted, not credited).
"""
import collections, os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ft8_vs_wsjtx as V

for path in sys.argv[1:]:
    runs = collections.defaultdict(lambda: {"BASE": set(), "PIECE": set()})
    cur = None
    for l in open(path, encoding="utf-8", errors="replace"):
        f = l.rstrip("\n").split("\t")
        if f[0] == "FILE":
            cur = f[1]; runs[cur]; continue
        if f[0] in ("BASE", "PIECE") and cur:
            runs[cur][f[0]].add(V.norm(f[1]))
    tot = collections.Counter()
    for wav, r in runs.items():
        jp, jap, _ = V.jt9(V.REF_EXE, os.path.abspath(wav), 3, "scratchpad/jt9wd")
        ref = jp | jap
        b, p = r["BASE"], r["PIECE"]
        tot["jt9"] += len(jp)
        tot["base"] += len(b); tot["base_ok"] += len(b & ref)
        tot["piece"] += len(p); tot["piece_ok"] += len(p & ref)
        u = b | p
        tot["union"] += len(u); tot["union_ok"] += len(u & ref)
        tot["piece_only_ok"] += len((p - b) & ref); tot["piece_only_bad"] += len((p - b) - ref)
        tot["base_only_ok"] += len((b - p) & ref)
    n = len(runs)
    print(f"{os.path.basename(path)}: {n} slots, jt9 plain {tot['jt9']/n:.2f}/slot")
    for k in ("base", "piece", "union"):
        print(f"  {k:6} {tot[k]:5} ({tot[k]/n:5.2f}/slot)  confirmed {tot[k+'_ok']:5}  not in jt9 {tot[k]-tot[k+'_ok']}")
    print(f"  piece adds {tot['piece_only_ok']} confirmed, {tot['piece_only_bad']} unconfirmed; "
          f"base-only confirmed {tot['base_only_ok']}")
