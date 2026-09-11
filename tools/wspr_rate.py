#!/usr/bin/env python3
"""Read the WSPR/FT8 capture-arm rate out of a serial capture (#376).

The pre-ring is fed from the USB isochronous stream, so a cycle in which the
CPU was too busy to service that endpoint arrives SHORT - silently, because
isochronous has no retry.  Nominal is 12.000 samples/ms.  A cycle at 11.90 has
lost 0.8 % of its audio, which over a 110.6 s WSPR transmission is ~0.9 s of
accumulated slip: about one whole symbol, and unrecoverable by any start-time
search, because it is a RATE error and not an offset.

Usage:  python tools/wspr_rate.py scratchpad/capture-376.txt
"""
import re
import sys

ARM = re.compile(r"\((\d+)\)\s+dsp: FT8 arm: head=(\d+) bf=(\d+) start=(\d+)"
                 r"(?: rate=(\d+)\.(\d+) smp/ms)?")
DEC = re.compile(r"\((\d+)\).*wspr.*?(\d+) decode")


def main(path):
    rows = []
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = ARM.search(line)
            if m:
                rows.append(tuple(int(x) for x in m.groups()[:4]) +
                            ((int(m.group(5)) * 1000 + int(m.group(6)),)
                             if m.group(5) else (None,)))
    if not rows:
        print("no 'FT8 arm:' lines found")
        return 1
    print("  t(ms)      head      bf   start-delta   rate smp/ms")
    prev = None
    rates = []
    for r in rows:
        t, head, bf, start, logged = r
        rate = ""
        if prev:
            dt = t - prev[0]
            if dt > 0:
                v = (head - prev[1]) / float(dt)
                rates.append(v)
                rate = "%8.3f%s" % (v, "  <-- AUDIO LOST" if v < 11.95 else "")
        ds = (start - prev[3]) if prev else 0
        print("%8d %9d %7d %10d %s%s" % (t, head, bf, ds, rate,
              ("   [logged %.3f]" % (logged / 1000.0)) if logged else ""))
        prev = r
    if rates:
        print("\ncycles=%d  mean=%.3f  min=%.3f  smp/ms  (nominal 12.000)"
              % (len(rates), sum(rates) / len(rates), min(rates)))
        loss = (12.0 - sum(rates) / len(rates)) / 12.0
        print("mean audio loss %.2f %%  =>  %.2f s of slip over a 110.6 s "
              "WSPR transmission (%.2f symbols)"
              % (loss * 100, loss * 110.6, loss * 110.6 / 0.6827))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1
                  else "scratchpad/capture-376.txt"))
