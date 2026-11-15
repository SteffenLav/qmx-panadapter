"""RTTY keying timing from a recording of a received over (mono WAV).

    python tools/rtty_edges.py over.wav --mark 1577 --space 1407 [--baud 45.45]

Every interval between two mark/space edges must be a whole number of HALF
bits (the stop bit is 1.5 bits). Prints the residual from that, which is the
keying jitter as the receiver saw it.

⛔ Do not measure edges against a fixed bit grid: after each 1.5-bit stop bit
the grid moves by half a bit, and every edge then reads ±T/2 off - a first
version did exactly that on 2026-11-15 and showed ±11 ms on a clean over.

Reference (bench1, 2026-11-15, 45.45 Bd, laptop QMX + dummy load):
  one-request CQ over      p95 1.5 ms, 1.6 % of intervals > 2.5 ms (fldigi clean)
  typed web over, prio 5   p95 3.9 ms, 11 %  of intervals > 2.5 ms (fldigi lost LTRS + space)
"""
import argparse
import wave

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("wav")
ap.add_argument("--mark", type=float, required=True)
ap.add_argument("--space", type=float, required=True)
ap.add_argument("--baud", type=float, default=45.45)
ap.add_argument("--limit", type=float, default=2.5, help="ms, intervals listed above this")
a = ap.parse_args()

w = wave.open(a.wav)
fs = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32)
if w.getnchannels() > 1:
    x = x[::w.getnchannels()]
t = np.arange(len(x)) / fs
k = int(fs * 0.003)                                    # 3 ms boxcar per tone


def env(f):
    c = np.cumsum(x * np.exp(-2j * np.pi * f * t))
    return np.abs(c[k:] - c[:-k])


m, s = env(a.mark), env(a.space)
on = (m + s) > 0.2 * np.max(m + s)                     # signal present
b = (m > s).astype(int)
idx = np.where(on[1:] & on[:-1] & (b[1:] != b[:-1]))[0]
et = []
for i in idx:                                          # debounce 4 ms
    if et and (i - et[-1]) < fs * 0.004:
        et[-1] = i
        continue
    et.append(i)
et = np.array(et) / fs
iv = np.diff(et)
half = 1.0 / a.baud / 2
n = np.round(iv / half)
keep = (n >= 1) & (n <= 40)
r = (iv - n * half)[keep] * 1000
tt = et[1:][keep]
bad = np.abs(r) > a.limit
print(f"{a.wav}: {len(r)} intervals  |residual| ms p50 {np.median(np.abs(r)):.2f} "
      f"p95 {np.percentile(np.abs(r), 95):.2f} max {np.max(np.abs(r)):.2f}  "
      f"> {a.limit} ms: {bad.sum()} ({100 * bad.mean():.1f} %)")
for i in np.where(bad)[0]:
    print(f"  t {tt[i]:7.3f} s  residual {r[i]:+6.2f} ms  ({int(n[keep][i])} half-bits)")
