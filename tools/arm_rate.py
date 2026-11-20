#!/usr/bin/env python3
"""Capture-rate reader: how much of the audio actually arrived.

The firmware prints, once per FT8/JS8 slot arm (main/dsp/dsp.c):

    FT8 arm: head=N bf=N start=N rate=11.999 smp/ms filled=N ms

`rate` is milli-samples per millisecond of audio that ARRIVED, with the time
base repair's fill taken back out. Nominal is 12.000. The firmware labels its
own failures `<-- AUDIO LOST` below 11.950.

[*] WHY THIS NUMBER AND NOT A DECODE COUNT. A shortfall here is a RATE error,
not an offset: 0.5 % over a 110.6 s WSPR transmission is 0.54 s of slip, about
0.8 of a symbol, and a 162-symbol matched filter then walks off its own tones
no matter where it starts. That is what holds WSPR_DEEP_MAX_PER_CYCLE at 0
(main/wspr_decode.c). A decode count tells you something went wrong; this
tells you what.

It is also the one instrument that reads the same on every board, because
the line comes from the firmware itself. That is the whole point: one
script, any capture file, no argument about method.

Usage:
    python tools/arm_rate.py scratchpad/capture-bench1.txt [more.txt ...]
    python tools/arm_rate.py --tail-mb 200 scratchpad/capture-bench1.txt

[!] TWO KINDS OF LINE ARE NOT MEASUREMENTS, and both are reported, never
dropped in silence:
  - rate=0.000 is the FIRST arm of a boot. There is no previous timestamp to
    divide by, so the instrument has not started yet.
  - rate above ~13 is a stale previous timestamp - an arm after the capture
    was paused, the tab was hidden, or FT8 was left and re-entered. The
    divisor is wall-clock, so a tiny gap gives a huge quotient.
Counting either as data moves the mean by orders of magnitude: on bench1's
own 200 MB tail the raw mean reads 75.5 smp/ms and the real one is 11.9996.
"""
import argparse
import re
import sys
from pathlib import Path

ARM = re.compile(r"FT8 arm:.*?rate=(\d+\.\d+) smp/ms")
BOOT = re.compile(r"Panadapter starting")
LOST_BELOW = 11.950
OUTLIER_ABOVE = 13.0


def read(path, tail_mb):
    data = Path(path).read_bytes()
    if tail_mb and len(data) > tail_mb * 1048576:
        data = data[-tail_mb * 1048576:]
    return data.decode("utf-8", "replace").splitlines()


def stats(v):
    n = len(v)
    if not n:
        return None
    m = sum(v) / n
    sd = (sum((x - m) ** 2 for x in v) / n) ** 0.5
    s = sorted(v)
    pct = lambda p: s[min(n - 1, int(n * p))]
    return dict(n=n, mean=m, sd=sd, min=s[0], p05=pct(.05), p50=pct(.50),
                p95=pct(.95), max=s[-1])


def last_boots(lines, n):
    """Keep only the last n boots.

    [!] THE WHOLE FILE IS NOT A BASELINE. capture-bench1.txt is ~1 GB and
    1316 boots deep, and it contains the builds that were DELIBERATELY
    starved - the WSPR deep-pass rations, the 3-minute carpet - whose whole
    purpose was to make this number bad. Averaging those in with today's
    firmware produces a figure that describes no build that ever shipped.
    Compare against the RECENT boots of current main."""
    if not n:
        return lines
    starts = [i for i, ln in enumerate(lines) if BOOT.search(ln)]
    return lines[starts[-n]:] if len(starts) >= n else lines


def report(name, lines):
    boots, rates, first, outl = 0, [], 0, []
    for ln in lines:
        if BOOT.search(ln):
            boots += 1
        m = ARM.search(ln)
        if not m:
            continue
        r = float(m.group(1))
        if r == 0.0:
            first += 1
        elif r > OUTLIER_ABOVE:
            outl.append(r)
        else:
            rates.append(r)

    print(f"\n=== {name}")
    print(f"boots seen: {boots}")
    if not rates:
        print("NO USABLE ARM LINES. Either FT8/JS8 was never entered in this")
        print("capture, or the capture is not of a running system. [!] An empty")
        print("instrument reads exactly like a healthy quiet one - do not")
        print("report this as 'no problem found'.")
        return None
    st = stats(rates)
    lost = [r for r in rates if r < LOST_BELOW]
    labelled = sum(1 for ln in lines if "AUDIO LOST" in ln)
    print(f"usable arms: {st['n']}   "
          f"(+{first} first-of-boot, +{len(outl)} stale-timestamp, both excluded)")
    # Cross-check against the firmware's OWN verdict. If these two disagree,
    # this script's banding is wrong, not the firmware's - it has the raw
    # integer and prints the label from it. Measured agreeing exactly on
    # bench1's capture, 2026-11-20.
    if labelled != len(lost):
        print(f"[!] BANDING DISAGREES WITH THE FIRMWARE: it labelled "
              f"{labelled} lines AUDIO LOST, this script counts {len(lost)}. "
              f"Trust the firmware and fix this script.")
    print(f"mean {st['mean']:.4f}  sd {st['sd']:.4f}  smp/ms   (nominal 12.000)")
    print(f"min {st['min']:.3f}  p05 {st['p05']:.3f}  p50 {st['p50']:.3f}  "
          f"p95 {st['p95']:.3f}  max {st['max']:.3f}")
    print("  [!] READ THE PERCENTILES, NOT THE MEAN. One stale-timestamp arm"
          " at 3.4 smp/ms moves the mean of 189 samples by 0.05 and the SD by"
          " 0.6; p05/p50/p95 do not move at all. The mean is printed so a"
          " GROSS shift is visible, not as the summary.")
    print(f"below {LOST_BELOW} (AUDIO LOST): {len(lost)} "
          f"= {100.0*len(lost)/st['n']:.2f} %")
    if lost:
        print("  worst: " + ", ".join(f"{r:.3f}" for r in sorted(lost)[:8]))
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--tail-mb", type=int, default=0,
                    help="read only the last N MB (a bench capture is ~1 GB)")
    ap.add_argument("--last-boots", type=int, default=0,
                    help="keep only the last N boots - see last_boots()")
    a = ap.parse_args()

    out = [(Path(f).name,
            report(Path(f).name, last_boots(read(f, a.tail_mb), a.last_boots)))
           for f in a.files]

    good = [(n, s) for n, s in out if s]
    if len(good) == 2:
        (n1, s1), (n2, s2) = good
        d = s2["mean"] - s1["mean"]
        # Standard error of the difference of two independent means.
        se = (s1["sd"] ** 2 / s1["n"] + s2["sd"] ** 2 / s2["n"]) ** 0.5
        print(f"\n=== {n2} minus {n1}")
        print(f"difference of means: {d:+.4f} smp/ms   "
              f"({d/12.0*100:+.3f} % of nominal)")
        if se > 0:
            print(f"standard error {se:.4f}  ->  {abs(d)/se:.1f} SE")
            print("[!] SE assumes the arms are independent. They are not quite:")
            print("  a busy minute starves several arms together. Read a result")
            print("  under about 3 SE as 'no difference shown', not as a tie.")
    elif len(good) > 2:
        print("\n(more than two usable files - compare them in pairs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
