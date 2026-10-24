#!/usr/bin/env python3
"""T57 - does the WSPR deep search BUY decodes, and does it still starve the capture?

The deep pass (#367/#376) has shipped OFF since 2026-09-09 -
WSPR_DEEP_MAX_PER_CYCLE 0 - and the reason was never its own cost. At ration
14 the decoder starved the capture task: the audio came up 0.49 % SHORT, and a
missing fraction of the audio is a RATE error, so a 162-symbol matched filter
walks off its own tones and every candidate fails at every dt. The decoder
goes deaf rather than slow.

So two questions, in this order:

  1. Does that starvation still happen on today's build? Watch dsp's capture
     line: head/elapsed against a nominal 12.000 samples/ms.
  2. If not, does the deep pass actually find more stations? That needs the
     two states on the SAME band in the SAME hour - propagation moves far more
     than the decoder does - which is why this alternates EVERY CYCLE instead
     of running two blocks.

It drives only {"action":"wspr_guards","deep":N}, a dev action with no TX and
no flash, and it puts the ration back to 0 when it exits, including on Ctrl-C.

Usage:
    python tools/wspr_deep_ab.py --host 192.168.1.211 \
        --capture scratchpad/capture-dev.txt --ration 14 --minutes 120

⚠ READ THE CAPTURE, NOT THE API, for the result: the decode counts are in
wspr_rx's own per-cycle line and the sample rate is in dsp's. This script
tallies them for you at the end, but the capture is the record.

⚠ The capture file spans MANY BOOTS (186 of them in the 709 MB file on
2026-10-22) and the uptime counter restarts at each one, so a plain timestamp
filter silently mixes sessions - it read an sd of 0.046 samples/ms where the
live boot alone reads 0.018. Everything below uses the FINAL MONOTONIC RUN.
"""
import argparse
import json
import re
import statistics as st
import sys
import time
import urllib.request

HEAD = re.compile(rb"I \((\d+)\) dsp: capture \(\d+ s\): \d+ iters \d+ smp head=(\d+)")
CYCLE = re.compile(rb"I? ?\((\d+)\) wspr_rx: cycle (\d+): (\d+) candidate\(s\), (\d+) decode\(s\)")

NOMINAL_SMP_PER_MS = 12.0


def post(host, body):
    req = urllib.request.Request("http://%s/api/cmd" % host,
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.load(r)


def tail(path, nbytes=60_000_000):
    with open(path, "rb") as f:
        try:
            f.seek(-nbytes, 2)
        except OSError:
            f.seek(0)
        return f.read()


def live_run(pairs):
    """Keep only the final monotonic run - see the warning in the docstring."""
    if not pairs:
        return []
    run = [pairs[-1]]
    for p in reversed(pairs[:-1]):
        if p[0] < run[0][0]:
            run.insert(0, p)
        else:
            break
    return run


def capture_rate(blob, lo_t, hi_t):
    rows = live_run([(int(m.group(1)), int(m.group(2))) for m in HEAD.finditer(blob)])
    out = []
    for (t0, h0), (t1, h1) in zip(rows, rows[1:]):
        if not (lo_t <= t0 < hi_t):
            continue
        dt, dh = t1 - t0, h1 - h0
        if 9000 < dt < 12000 and dh > 0 and 10 < dh / dt < 14:
            out.append(dh / dt)
    return out


def cycles(blob):
    rows = live_run([(int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))
                     for m in CYCLE.finditer(blob)])
    return rows


def report(label, rates, decs):
    if rates:
        loss = (st.mean(rates) / NOMINAL_SMP_PER_MS - 1.0) * 100.0
        print("  %-9s capture %d windows  mean %.4f smp/ms  sd %.4f  min %.3f  (%+.3f %% vs nominal)"
              % (label, len(rates), st.mean(rates), st.pstdev(rates), min(rates), loss))
    if decs:
        print("  %-9s cycles  %d   decodes %d   mean %.2f/cycle   %s"
              % (label, len(decs), sum(decs), sum(decs) / len(decs), decs))


def main():
    ap = argparse.ArgumentParser()
    # ⚠ A hard-coded IP here goes stale silently - this said .211 while the
    # board was on .209, which is a timeout and not an error message.
    # tools/bench.json is the registry; the board prints its own address in
    # its capture ("sta ip:"). Pass --host when in doubt.
    ap.add_argument("--host", default="192.168.1.209")
    ap.add_argument("--capture", default="scratchpad/capture-dev.txt")
    ap.add_argument("--ration", type=int, default=14)
    ap.add_argument("--minutes", type=int, default=120)
    a = ap.parse_args()

    # Mark the start against the DEVICE's clock, not the wall clock: everything
    # parsed below is in uptime ms.
    blob = tail(a.capture, 4_000_000)
    rows = live_run([(int(m.group(1)), int(m.group(2))) for m in HEAD.finditer(blob)])
    if not rows:
        sys.exit("no dsp capture lines in %s - is the capture running?" % a.capture)
    t_start = rows[-1][0]
    print("start at device t=%d ms, alternating 0 / %d every WSPR cycle for %d min"
          % (t_start, a.ration, a.minutes))

    # WSPR cycles are 120 s. Flip just after a boundary so a cycle is decoded
    # entirely under one setting; the ration is re-read at each cycle's start.
    marks = []          # (t_at_flip, ration)
    deadline = time.time() + a.minutes * 60
    state = 0
    try:
        while time.time() < deadline:
            state = a.ration if state == 0 else 0
            post(a.host, {"action": "wspr_guards", "deep": state})
            blob = tail(a.capture, 2_000_000)
            r = live_run([(int(m.group(1)), int(m.group(2))) for m in HEAD.finditer(blob)])
            marks.append((r[-1][0] if r else None, state))
            print("  deep=%-3d at device t=%s" % (state, marks[-1][0]))
            time.sleep(120)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        post(a.host, {"action": "wspr_guards", "deep": 0})
        print("ration restored to 0")

    blob = tail(a.capture)
    cyc = cycles(blob)
    marks = [m for m in marks if m[0] is not None]
    print("\nresults (device t >= %d):" % t_start)
    for i, (t0, ration) in enumerate(marks):
        t1 = marks[i + 1][0] if i + 1 < len(marks) else 10 ** 12
        decs = [d for (t, _c, _n, d) in cyc if t0 <= t < t1]
        report("deep=%d" % ration, capture_rate(blob, t0, t1), decs)


if __name__ == "__main__":
    main()
