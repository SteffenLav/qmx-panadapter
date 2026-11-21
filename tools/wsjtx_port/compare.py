"""The WS 3.2.1 FT8 port (main/ft8_wsjtx.c) against WS 3.2.1's own jt9, slot by slot.

Done means: the same plain-decode set as jt9 -d 3 on every slot. This script
prints totals, the per-slot differences, and the time per slot.

  python tools/wsjtx_port/compare.py <dir of .wav> [--jobs 8] [--flags=...]

Our output per slot is cached in <dir>/wsx_out/<wav>.txt only for this run
(rebuilt each time); jt9's cache is <wav>.ws321d3.txt (tools/ft8_vs_wsjtx.py).
"""
import argparse, concurrent.futures as cf, glob, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ft8_vs_wsjtx as V  # noqa: E402

F = "components/ft8_lib"
SRCS = ["test/ft8_wsjtx_harness.c", "main/ft8_wsjtx.c"] + [
    f"{F}/ft8/{n}.c" for n in ("encode", "message", "text", "crc", "constants", "js8_tables")] + [
    f"{F}/fft/kiss_fft.c", f"{F}/fft/kiss_fftr.c"]
INC = ["-Imain", f"-I{F}", f"-I{F}/ft8", f"-I{F}/fft", f"-I{F}/common"]


def build(exe, flags):
    subprocess.run(["gcc", "-O2", "-DFT8_DECODER_WSJTX", *flags, "-o", exe, *SRCS, *INC, "-lm"], cwd=ROOT, check=True)


def run(exe, wav):
    t0 = time.perf_counter()
    out = subprocess.run([exe, wav], capture_output=True, text=True).stdout
    dt = time.perf_counter() - t0
    plain, ap = set(), set()
    for l in out.splitlines():
        f = l.split("\t")
        if len(f) >= 3:
            (ap if f[0] == "APMSG" else plain).add(V.norm(f[2]))
    return wav, plain, ap, dt


def main():
    a = argparse.ArgumentParser()
    a.add_argument("dir")
    a.add_argument("--jobs", type=int, default=8)
    a.add_argument("--flags", default="")
    a.add_argument("--show", type=int, default=20)
    args = a.parse_args()
    wavs = sorted(os.path.abspath(w) for w in glob.glob(os.path.join(args.dir, "*.wav")))
    exe = os.path.join(ROOT, "scratchpad", "wsx_cmp_single.exe")  # not wsx_cmp.exe: a multi-file run may be using that one
    build(exe, args.flags.split())
    wd = os.path.join(ROOT, "scratchpad", "jt9wd")
    os.makedirs(wd, exist_ok=True)
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        res = list(ex.map(lambda w: run(exe, w), wavs))
    ap_o = ap_j = ap_both = 0
    n_o = n_j = both = 0
    t = 0.0
    same = 0
    diffs = []
    for wav, plain, ap, dt in res:
        jp, jap, _ = V.jt9(V.REF_EXE, wav, 3, wd)
        t += dt
        ap_o += len(ap); ap_j += len(jap); ap_both += len(ap & jap)
        n_o += len(plain); n_j += len(jp); both += len(plain & jp)
        if plain == jp:
            same += 1
        else:
            diffs.append((os.path.basename(wav), sorted(plain - jp), sorted(jp - plain)))
    n = len(wavs)
    if n_o == 0 or n_j == 0:
        print("INSTRUMENT FAIL: nothing decoded"); return 1
    print(f"{n} slots: port {n_o} ({n_o/n:.2f}/slot)  jt9 {n_j} ({n_j/n:.2f}/slot)  both {both}")
    print(f"  identical slots {same}/{n}; port-only {n_o-both}, jt9-only {n_j-both}")
    print(f"  AP decodes: port {ap_o}, jt9 {ap_j}, both {ap_both}")
    print(f"  port {1000*t/n:.0f} ms/slot (wall, {args.jobs} in parallel)")
    for f, oo, jj in diffs[:args.show]:
        print(f"  {f}: +{oo} -{jj}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
