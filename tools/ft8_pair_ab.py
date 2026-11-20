"""Pair two FT8 decoder builds on the SAME audio and diff what each decodes.

Why: a change measured on air in alternating blocks compares two different
bands (2026-11-20: dup-subtract was +4 ground-truth messages on the host corpus
and LOWER on air in two activity bins). Run on identical recorded slots, the
only difference left is the code. With no truth file the comparison is the
per-slot message sets: what A decodes that B does not, and the reverse.

Builds test/ft8_osd_proto_harness.c twice with -DPRINT_MSGS:
  A = BASE flags            (dup instances subtracted - the decoder today)
  B = BASE + -DSUB_SKIP_DUP (first instance only - the old behaviour)
BASE matches the off-device build: 2 passes, OSD order 0, ndis <= 34.

Instrument checks, run every time before any A/B figure:
  1. A against A must differ on NOTHING (determinism) - it stops if it does.
  2. On test/wav_reference (truth files present) it PRINTS A's and B's
     ground-truth hits. Measured 2026-11-21 with BASE: A 285, B 282. A run
     that prints anything else has a different decoder from that one.

A message "also decoded in another slot" is a plausibility mark only. In
test/wav_reference the _weak files are the SAME audio at a lower level, so
there a repeat proves less than it does across consecutive live slots.

Usage:
  python tools/ft8_pair_ab.py <dir of .wav> [more dirs]
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
        "-DNPASS=2", "-DOSD_ORDER=0", "-DOSD_MAX_NDIS=34"]


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
    dirs = sys.argv[1:]
    if not dirs:
        print(__doc__); return 2
    with tempfile.TemporaryDirectory() as tmp:
        exe_a = build(tmp, "A", [])
        exe_b = build(tmp, "B", ["-DSUB_SKIP_DUP"])

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
        for tag, lst in (("A only", oa), ("B only", ob)):
            rep = sum(1 for _, t in lst if seen[t] > 1)
            print(f"  {tag}: {len(lst)}, of which also decoded in another slot: {rep}")
            for f, t in lst[:40]:
                print(f"    {f:24} {t}{'   (repeats)' if seen[t] > 1 else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
