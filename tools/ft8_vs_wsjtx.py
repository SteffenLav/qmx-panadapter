"""Our FT8 decoder against WSJT-X's jt9, on the SAME recorded slots.

Both decoders read the identical 12 kHz WAV, so the band is not a variable:
what one finds and the other misses is the decoders. Ours is built from
test/ft8_osd_proto_harness.c with the given flags (default: the off-device
build - 2 passes, OSD order 0 without screen, 200 candidates).

  python tools/ft8_vs_wsjtx.py <dir of .wav> [--jt9 PATH] [--depth 1|2|3]

Reported per side: messages, messages the other side lacks, time per slot.
jt9 marks a priori (AP) decodes with a trailing "a1".."a7": those are counted
separately, because AP fills in bits from what it EXPECTS and is a different
kind of claim from a decode that stands on the signal alone.

Times are wall clock per slot including process start-up, single run. jt9's
-d 3 is its deepest search; it is what WSJT-X runs at "Deep".
"""
import argparse, glob, os, re, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ft8_pair_ab as P  # noqa: E402  (shares the harness build)


# The referee. Since 2026-11-21 it is WS 3.2.1 (WSJT-X Improved, renamed), the
# version the decoder port follows; the old WSJT-X Improved 3.0.0 251101 jt9 is
# --jt9 C:/WSJT/wsjtx/bin/jt9.exe --ref-tag jt9. Each referee caches under its
# own tag, so the two can never be mixed up in a cache file.
REF_EXE = os.path.join(ROOT, "scratchpad", "ws-3.2.1-win", "bin_x", "bin", "jt9.exe")
REF_TAG = "ws321"


def norm(t):
    return " ".join(t.upper().split())


def ours(exe, wav):
    t0 = time.perf_counter()
    out = subprocess.run([exe, wav, os.devnull], cwd=ROOT, capture_output=True, text=True).stdout
    dt = time.perf_counter() - t0
    return {norm(l.split("\t", 2)[2]) for l in out.splitlines() if l.startswith("MSG\t")}, dt


def jt9(path, wav, depth, wd, tag=None):
    """jt9's decodes of one slot. Its raw output is CACHED beside the WAV
    (<wav>.<tag>d<depth>.txt), so a slot costs its ~3 s once and later runs -
    and ft8_pair_ab.py --jt9 - can use jt9 as the reference for free."""
    tag = tag or REF_TAG
    cache = f"{wav}.{tag}d{depth}.txt"
    t0 = time.perf_counter()
    if os.path.exists(cache):
        out = open(cache, encoding="utf-8").read()
    else:
        out = subprocess.run([path, "-8", "-d", str(depth), wav], cwd=wd,
                             capture_output=True, text=True).stdout
        if "<DecodeFinished>" in out:          # only a complete run is cached
            open(cache, "w", encoding="utf-8").write(out)
    dt = time.perf_counter() - t0
    plain, ap = set(), set()
    for l in out.splitlines():
        m = re.match(r"^\s*\d{6}\s+-?\d+\s+-?[\d.]+\s+\d+\s+~\s+(.*?)\s*$", l)
        if not m:
            continue
        txt = m[1]
        # AP lines end "aN", and jt9 puts "?" before it when qual < 0.17
        # (decoder.f90). Left in, the "?" made 58 matching AP lines differ.
        a = re.search(r"(\s+\?)?\s+a\d\s*$", txt)
        (ap if a else plain).add(norm(txt[:a.start()] if a else txt))
    return plain, ap, dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--jt9", default=REF_EXE)
    ap.add_argument("--ref-tag", default=REF_TAG, help="cache tag of this referee")
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--flags", default="-DMAXC=200",
                    help="extra gcc flags for our decoder (use --flags=...)")
    a = ap.parse_args()
    # ABSOLUTE: jt9 runs in its own scratch folder, and a relative path there
    # found nothing - 0 decodes in 26 ms a slot, which read like a result.
    wavs = sorted(os.path.abspath(w) for w in glob.glob(os.path.join(a.dir, "*.wav")))
    if not wavs:
        print("no .wav in", a.dir); return 1
    with tempfile.TemporaryDirectory() as tmp:
        exe = P.build(tmp, "ours", a.flags.split())
        wd = os.path.join(tmp, "jt9"); os.makedirs(wd)
        n_o = n_j = n_ap = only_o = only_j = both = 0
        t_o = t_j = 0.0
        ex_o, ex_j = [], []
        for w in wavs:
            o, to = ours(exe, w)
            j, japs, tj = jt9(a.jt9, w, a.depth, wd, a.ref_tag)
            t_o += to; t_j += tj
            n_o += len(o); n_j += len(j); n_ap += len(japs - o)
            both += len(o & j)
            oo, jj = o - j - japs, j - o
            only_o += len(oo); only_j += len(jj)
            ex_o += [(os.path.basename(w), t) for t in sorted(oo)]
            ex_j += [(os.path.basename(w), t) for t in sorted(jj)]
        n = len(wavs)
        if n_j == 0 or n_o == 0:
            print(f"INSTRUMENT FAIL: {'jt9' if n_j == 0 else 'ours'} decoded nothing in "
                  f"{n} slots - a broken run, not a result")
            return 1
        print(f"{n} slots, jt9 depth {a.depth}, ours {a.flags}")
        print(f"  ours : {n_o:5} messages ({n_o/n:.1f}/slot), {only_o} that jt9 lacks, "
              f"{1000*t_o/n:.0f} ms/slot")
        print(f"  jt9  : {n_j:5} messages ({n_j/n:.1f}/slot), {only_j} that ours lacks, "
              f"{1000*t_j/n:.0f} ms/slot  (+{n_ap} AP-only, not counted)")
        print(f"  both : {both}")
        for tag, lst in (("ours only", ex_o), ("jt9 only", ex_j)):
            print(f"\n{tag} (first 15):")
            for f, t in lst[:15]:
                print(f"    {f:24} {t}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
