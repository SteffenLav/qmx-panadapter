#!/usr/bin/env python3
"""T36 - what the rate loop's measuring window does to playback PITCH.

rx_audio.c computes the playback rate from a count of delivered pairs over a
window, so the window's measurement noise IS pitch noise. This replays the
REAL delivery rate recorded in a serial capture through the loop's own
arithmetic and prints the resulting pitch spread for several window lengths.

Input: any capture containing audio.c's per-second line

    I (12345) audio: RX 47891 pairs/s peak L=20 R=20  (mean 48021 over 10 s)

Usage:
    python tools/resample_window_sim.py scratchpad/capture-bench1.txt

⚠ WHAT THIS IS NOT. audio.c's counter counts pairs DELIVERED by the UAC
driver; rx_audio.c's window counts pairs CONSUMED by rx_audio_task, over a
window whose edges are its own frame boundaries. They are two counters over
one stream, so this is a LOWER BOUND on the shipped figure: the consumer's own
scheduling adds noise this replay cannot see. The healthy-band filter below
discards the tails as well, and those rail hardest.

The quantisation this measures is not noise in the radio. One USB transfer is
40 packets = 1920 pairs (40 ms), so a window catches a whole number of
transfers depending on where its edges fall: a PERFECT 48 kHz stream reads
46080/48000/49920 over 1 s. audio.c's own comment has carried that number for
a year; T36 is what it does once the figure is used as a playback rate.
"""
import re
import statistics as st
import sys

# audio.c logs the per-second count on the "quiet" line, which also carries the
# 10 s mean. Anything outside this band was printed BECAUSE it was abnormal
# (its `odd` test is <46000 or >50000), so including it would bias the sample
# toward the outliers rather than describe ordinary seconds.
HEALTHY_LO, HEALTHY_HI = 46000, 50000

LINE = re.compile(rb"RX (\d+) pairs/s[^(]*\(mean (\d+) over 10 s")

D = 4.0          # RX_DECIM_D_AM - any D cancels; the result is in ppm
FS = 48000.0     # DSP_SAMPLE_RATE_HZ
NOM = 1.0 / D    # s_up_inc with no correction

# (window seconds, smoothing alpha). The first row is what shipped to v1.20.3.
CASES = [(1, 0.25), (10, 0.25), (10, 0.10), (30, 0.10)]


def read_rates(path):
    inst, mean = [], []
    with open(path, "rb") as f:
        for m in LINE.finditer(f.read()):
            i, mn = int(m.group(1)), int(m.group(2))
            # Both must be in band. The 10 s mean is printed on the same line
            # but is its OWN measurement: a line whose instantaneous count is
            # ordinary can still carry a mean from a window in which the stream
            # stopped, and one of those read -90 % and made the 10 s row look
            # worse than the 1 s row it exists to beat.
            if HEALTHY_LO <= i <= HEALTHY_HI and 47000 <= mn <= 49000:
                inst.append(i)
                mean.append(mn)
    return inst, mean


def replay(series, win_s, alpha):
    """rx_audio.c's own arithmetic, in ppm from nominal."""
    target = 0.0
    out = []
    for k in range(0, len(series) - win_s + 1, win_s):
        src_rate = sum(series[k:k + win_s]) / win_s
        t = src_rate / (D * FS)
        target = t if target <= 0.0 else target + (t - target) * alpha
        out.append((target / NOM - 1.0) * 1e6)
    return out[20:]          # drop the acquisition transient


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    inst, mean = read_rates(sys.argv[1])
    if len(inst) < 600:
        sys.exit("only %d healthy seconds found - need a longer capture" % len(inst))

    print("%d healthy seconds of delivered rate\n" % len(inst))
    for name, v in (("1 s count", inst), ("10 s mean", mean)):
        sd = st.pstdev(v)
        print("  %-10s sd %4.0f pairs = %.2f %% of nominal   range %+.2f %% .. %+.2f %%"
              % (name, sd, sd / FS * 100, (min(v) - FS) / FS * 100, (max(v) - FS) / FS * 100))
    print("\n  the offset actually being corrected is 0.24 %\n")

    print("playback rate after the loop's smoothing:")
    for win_s, alpha in CASES:
        p = replay(inst, win_s, alpha)
        if not p:
            continue
        railed = sum(1 for x in p if abs(x) > 20000)   # the +/-2 % clamp
        print("  %2d s, alpha %.2f   sd %5.0f ppm (%.3f %%)   range %+7.0f .. %+7.0f ppm"
              "   past the clamp: %d/%d"
              % (win_s, alpha, st.pstdev(p), st.pstdev(p) / 1e4,
                 min(p), max(p), railed, len(p)))


if __name__ == "__main__":
    main()
