"""Pair two FT8 decoder builds on the SAME audio and diff what each decodes.

Why: a change measured on air in alternating blocks compares two different
bands (2026-11-20: dup-subtract was +4 ground-truth messages on the host corpus
and LOWER on air in two activity bins). Run on identical recorded slots, the
only difference left is the code. With no truth file the comparison is the
per-slot message sets: what A decodes that B does not, and the reverse.

Builds test/ft8_osd_proto_harness.c twice with -DPRINT_MSGS:
  A = BASE flags            (dup instances subtracted - the decoder today)
  B = BASE + -DSUB_SKIP_DUP (first instance only - the old behaviour)
BASE matches the off-device build: 2 passes, OSD order 0, no ndis screen.

Instrument checks, run every time before any A/B figure:
  1. A against A must differ on NOTHING (determinism) - it stops if it does.
  2. On test/wav_reference (truth files present) it PRINTS A's and B's
     ground-truth hits. Measured 2026-11-21 with BASE (no ndis screen), dup
     arms: A 288, B 282 (285 / 282 with the old ndis<=34 screen). A run
     that prints anything else has a different decoder from that one.

A message "also decoded in another slot" is a plausibility mark only. In
test/wav_reference the _weak files are the SAME audio at a lower level, so
there a repeat proves less than it does across consecutive live slots.

Usage:
  python tools/ft8_pair_ab.py <dir of .wav> [more dirs]          (dup arms)
  python tools/ft8_pair_ab.py DIR --a= --b=-DOSD_OFF              (OSD on/off)
  python tools/ft8_pair_ab.py DIR --a=-DMAXC=200 --b=-DMAXC=140
(write --a=FLAGS with "=": a value starting with "-" is otherwise read as an option)
--a/--b are extra gcc flags per arm; --maxc sets the cap for an arm that does
not set -DMAXC itself (default 140, the device's).
"""
import collections, glob, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
F = "components/ft8_lib"
SRCS = ["test/ft8_osd_proto_harness.c"] + [
    f"{F}/ft8/{n}.c" for n in ("decode", "encode", "ldpc", "text", "message", "crc",
                               "constants", "js8_codec", "js8_tables")] + [
    f"{F}/common/monitor.c", f"{F}/fft/kiss_fft.c", f"{F}/fft/kiss_fftr.c",
    "main/ft8_subtract.c"]
INC = ["-Imain", f"-I{F}", f"-I{F}/ft8", f"-I{F}/common", f"-I{F}/fft"]
BASE = ["-O2", "-DFTX_EXPOSE_LIKELIHOOD", "-DPRINT_MSGS",
        "-DNPASS=2", "-DOSD_ORDER=0", "-DOSD_MAX_NDIS=0"]   # = osd.h since 2026-11-21


def build(tmp, tag, extra):
    exe = os.path.join(tmp, f"pair_{tag}.exe")
    subprocess.run(["gcc", *BASE, *extra, "-o", exe, *SRCS, *INC, "-lm"],
                   cwd=ROOT, check=True)
    return exe


def run(exe, wav):
    truth = wav[:-4] + ".txt"
    out = subprocess.run([exe, wav, truth if os.path.exists(truth) else os.devnull],
                         cwd=ROOT, capture_output=True, text=True).stdout
    msgs = {l.split("\t", 2)[2].strip() for l in out.splitlines() if l.startswith("MSG\t")}
    m = re.search(r"truth=\s*(\d+) uniq=\s*(\d+) hit=\s*(\d+)", out)
    return msgs, (int(m[1]), int(m[2]), int(m[3])) if m else (0, 0, 0)


def pair(exe_a, exe_b, wavs):
    only_a, only_b, both = [], [], 0
    hits = [0, 0]
    for w in wavs:
        a, sa = run(exe_a, w)
        b, sb = run(exe_b, w)
        hits[0] += sa[2]; hits[1] += sb[2]
        both += len(a & b)
        only_a += [(os.path.basename(w), t) for t in sorted(a - b)]
        only_b += [(os.path.basename(w), t) for t in sorted(b - a)]
    return both, only_a, only_b, hits


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Pair two FT8 decoder builds on the same WAVs.")
    ap.add_argument("dirs", nargs="+", help="directories of .wav slots")
    ap.add_argument("--a", default="", help="extra gcc flags for arm A (default: none)")
    ap.add_argument("--b", default="-DSUB_SKIP_DUP", help="extra gcc flags for arm B")
    ap.add_argument("--maxc", type=int, default=140,
                    help="candidate cap for BOTH arms unless an arm sets -DMAXC itself")
    ap.add_argument("--jt9", type=int, default=0,
                    help="score each arm's extra messages against jt9 at this depth (1-3)")
    ap.add_argument("--jt9-exe", default=None, help="default: ft8_vs_wsjtx.REF_EXE")
    ap.add_argument("--ref-tag", default=None, help="default: ft8_vs_wsjtx.REF_TAG")
    args = ap.parse_args()
    dirs = args.dirs
    fa, fb = args.a.split(), args.b.split()
    for f in (fa, fb):
        if not any(x.startswith("-DMAXC=") for x in f):
            f.append(f"-DMAXC={args.maxc}")
    print(f"A: {' '.join(fa)}\nB: {' '.join(fb)}")
    with tempfile.TemporaryDirectory() as tmp:
        exe_a = build(tmp, "A", fa)
        exe_b = build(tmp, "B", fb)

        ref = sorted(glob.glob(os.path.join(ROOT, "test/wav_reference/*.wav")))
        ref = [w for w in ref if os.path.exists(w[:-4] + ".txt")]
        _, oa, ob, _ = pair(exe_a, exe_a, ref[:6])
        if oa or ob:
            print(f"INSTRUMENT FAIL: A vs A differs ({len(oa)}/{len(ob)})"); return 1
        _, _, _, hits = pair(exe_a, exe_b, ref)
        print(f"reference corpus ({len(ref)} files with truth): hits A={hits[0]} B={hits[1]}")

        wavs = sorted(w for d in dirs for w in glob.glob(os.path.join(d, "*.wav")))
        if not wavs:
            print("no .wav files in", dirs); return 1
        both, oa, ob, _ = pair(exe_a, exe_b, wavs)
        print(f"\n{len(wavs)} slots: both {both}, A only {len(oa)}, B only {len(ob)}")
        # A message seen in more than one slot is very likely a real station:
        # a 14-bit CRC false accept does not repeat itself.
        seen = collections.Counter()
        for w in wavs:
            seen.update(run(exe_a, w)[0] | run(exe_b, w)[0])
        # --jt9 D: WSJT-X's jt9 at depth D as the referee. A message jt9 also
        # decodes from the same slot is real; this replaces "repeats" as the
        # judge wherever it is available. Cached per slot by ft8_vs_wsjtx.
        ref_sets = {}
        if args.jt9:
            import ft8_vs_wsjtx as V
            wd = os.path.join(tmp, "jt9"); os.makedirs(wd, exist_ok=True)
            for w in wavs:
                plain, apd, _ = V.jt9(args.jt9_exe or V.REF_EXE, os.path.abspath(w), args.jt9, wd, args.ref_tag)
                ref_sets[os.path.basename(w)] = plain | apd
        for tag, lst in (("A only", oa), ("B only", ob)):
            rep = sum(1 for _, t in lst if seen[t] > 1)
            line = f"  {tag}: {len(lst)}, of which also decoded in another slot: {rep}"
            if ref_sets:
                inj = sum(1 for f, t in lst if " ".join(t.upper().split()) in ref_sets.get(f, ()))
                line += f", confirmed by jt9 -d {args.jt9}: {inj}"
            print(line)
            for f, t in lst[:40]:
                mark = ""
                if ref_sets:
                    mark = "   (jt9)" if " ".join(t.upper().split()) in ref_sets.get(f, ()) else "   (NOT in jt9)"
                print(f"    {f:24} {t}{'   (repeats)' if seen[t] > 1 else ''}{mark}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
