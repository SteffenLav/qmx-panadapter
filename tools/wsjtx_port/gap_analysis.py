"""Why does jt9 find decodes that the Tab5 (ft8_lib + K40 piece pass) does not?

    python tools/wsjtx_port/gap_analysis.py <piece_K40_harness_output.txt> <gap.exe>

For every slot in the harness output whose WAV has a cached jt9 result, each
jt9 decode that neither BASE nor PIECE produced is put in ONE class:

  ap_only      jt9 needed a priori info (aN line) - not in the piece pass
  no_cand      no ft8_lib candidate within 3.2 Hz / 0.16 s of jt9's f/DT
  cand_ok      the nearest candidate decoded to SOMETHING ELSE (a neighbour)
  beyond_k     a candidate exists, ft8_lib failed it, ranked past K=40
  tried_pos    tried, failed - but ft8b at jt9's EXACT f/DT decodes it
               (our candidate position is the limiter)
  tried_hard   tried, failed, and fails at jt9's exact f/DT too (needs what
               ft8b alone lacks: subtraction of stronger signals, a7/a8, ...)
"""
import collections
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import ft8_vs_wsjtx as V  # noqa: E402

K = int(os.environ.get("GAP_K", "60"))
DF, DDT = 3.2, 0.16


def hfold(t):
    return re.sub(r"<[^>]*>", "<H>", t)


def jt9_lines(wav):
    cache = f"{wav}.{V.REF_TAG}d3.txt"
    if not os.path.exists(cache):
        return None
    out = []
    for l in open(cache, encoding="utf-8"):
        m = re.match(r"^\s*\d{6}\s+(-?\d+)\s+(-?[\d.]+)\s+(\d+)\s+~\s+(.*?)\s*$", l)
        if not m:
            continue
        txt = m[4]
        a = re.search(r"(\s+\?)?\s+a\d\s*$", txt)
        out.append((float(m[3]), float(m[2]), int(m[1]), V.norm(txt[:a.start()] if a else txt), bool(a)))
    return out


def main():
    harness_out, exe = sys.argv[1], sys.argv[2]
    runs, cur = {}, None
    for l in open(harness_out, encoding="utf-8"):
        f = l.rstrip("\n").split("\t")
        if f[0] == "FILE":
            cur = f[1]; runs[cur] = set()
        elif f[0] in ("BASE", "PIECE") and cur:
            runs[cur].add(hfold(V.norm(f[1])))
    cls = collections.Counter()
    snr = collections.defaultdict(list)
    dlt = collections.defaultdict(list)
    nslots = 0
    for wav, ours in runs.items():
        j = jt9_lines(wav)
        if j is None:
            continue
        nslots += 1
        miss = [x for x in j if hfold(x[3]) not in ours]
        if not miss:
            continue
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as tf:
            for fq, dt, _, txt, _ in miss:
                tf.write(f"{fq} {dt} {txt}\n")
        out = subprocess.run([exe, wav, tf.name], capture_output=True, text=True).stdout
        os.unlink(tf.name)
        cands, exact = [], []
        for l in out.splitlines():
            p = l.split("\t")
            if p[0] == "CAND":
                cands.append((int(p[1]), float(p[2]), float(p[3]), int(p[4])))
            elif p[0] == "EXACT":
                exact.append(p[1] == "1")
        fails = [c for c in cands if not c[3]]
        frank = {c[0]: r for r, c in enumerate(fails)}
        for (fq, dt, s, txt, ap), ex in zip(miss, exact):
            if ap:
                c = "ap_only"
            else:
                near = [c for c in cands if abs(c[1] - fq) <= DF and abs(c[2] - dt) <= DDT]
                if not near:
                    c = "no_cand"
                else:
                    best = min(near, key=lambda c: c[0])
                    if best[3]:
                        failnear = [c for c in near if not c[3]]
                        if not failnear:
                            c = "cand_ok"
                            cls[c] += 1; snr[c].append(s)
                            continue
                        best = min(failnear, key=lambda c: c[0])
                    if frank[best[0]] >= K:
                        c = "beyond_k"
                    else:
                        c = "tried_pos" if ex else "tried_hard"
                        dlt[c].append((best[2] - dt, best[1] - fq))
            cls[c] += 1
            snr[c].append(s)
    tot = sum(cls.values())
    print(f"{nslots} slots, {tot} jt9 decodes we miss ({tot / max(1, nslots):.2f}/slot)")
    for k in ("ap_only", "no_cand", "cand_ok", "beyond_k", "tried_pos", "tried_hard"):
        v = snr[k]
        med = sorted(v)[len(v) // 2] if v else 0
        print(f"  {k:10} {cls[k]:5}  {100 * cls[k] / max(1, tot):5.1f} %   median SNR {med:+d} dB")
    for k in ("tried_pos", "tried_hard"):
        d = dlt[k]
        if not d:
            continue
        adt = sorted(abs(x[0]) for x in d); adf = sorted(abs(x[1]) for x in d)
        far = sum(1 for x in d if abs(x[0]) > 0.05)
        sdt = sorted(x[0] for x in d)
        print(f"  {k}: signed dt p10 {sdt[len(sdt)//10]*1000:+.0f} p50 {sdt[len(sdt)//2]*1000:+.0f} p90 {sdt[9*len(sdt)//10]*1000:+.0f} ms")
        print(f"  {k}: |cand dt - jt9 dt| p50 {adt[len(adt)//2]*1000:.0f} ms p90 {adt[int(.9*len(adt))]*1000:.0f} ms, "
              f"> 50 ms: {far} of {len(d)}; |df| p50 {adf[len(adf)//2]:.2f} p90 {adf[int(.9*len(adf))]:.2f} Hz")


if __name__ == "__main__":
    main()
