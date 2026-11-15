#!/usr/bin/env python3
"""RX audio health gate (milestone A, agreed with the operator 2026-11-15).

Reads a serial capture, splits it into boots, and judges each boot's rx_audio
10 s diag windows against A1-A5. The first 60 s after AUDIO START (first
window with OUT > 0) are excluded - the QMX power cycle after a flash comes
later than the Tab5 boot.

  A1 fwd drop = 0 in >= 99 % of windows; each drop window shows a stall
     (away max > 61 ms) and backlog < 6400 within 2 windows.
  A2 I2Sovf (codec underruns) rises in <= 1 % of windows.
  A3 |pitch| <= 3000 ppm every window; within +/-500 when backlog < 6400;
     no rail / re-lock lines.
  A4 OUT within +/-0.2 % of the USB rate, over 60 s sums. Not per 10 s:
     one USB chunk is 0.67 % of a 10 s window, so the per-window USB mean
     itself jitters +/-0.3 % (measured 2026-11-15).
  A5 backlog max < 12800; flushes only follow an underrun (since the
     underrun flush, a flush after A2's gap is intended).

Needs firmware >= 16dad59b (backlog + flush fields). Older boots are listed
as "no fields".

usage: audio_health.py CAPTURE [--boot SUBSTR] [--last N] [--min-windows N]
"""
import argparse
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
R_VER = re.compile(r"App version:\s+(\S+)")
R_USB = re.compile(r"audio: RX (\d+) pairs/s.*mean (\d+)")
R_DIAG = re.compile(r"\((\d+)\) rx_audio: diag: frame .*?OUT (\d+) smp/s.*?"
                    r"pitch ([+-]\d+) ppm.*?I2Sovf (\d+)")
R_WIN = re.compile(r"win fwd drop (\d+) pairs.*?away max (\d+) us.*?"
                   r"backlog max (\d+) now (\d+) pairs  flush (\d+)")
R_BAD = re.compile(r"hit the rail|re-locking|persists - accepting")

DEADBAND, FLUSH_AT, SLACK_US = 6400, 12800, 61000


def boots(path):
    cur = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = ANSI.sub("", line)
            m = R_VER.search(line)
            if m:
                cur = {"ver": m[1], "w": [], "bad": 0, "usb": None, "pend": None}
                yield cur
                continue
            if cur is None:
                continue
            m = R_USB.search(line)
            if m:
                cur["usb"] = int(m[2])
                continue
            m = R_DIAG.search(line)
            if m:
                cur["pend"] = {"t": int(m[1]) / 1000, "out": int(m[2]),
                               "ppm": int(m[3]), "ovf": int(m[4]), "usb": cur["usb"]}
                continue
            m = R_WIN.search(line)
            if m and cur["pend"]:
                w = cur["pend"]
                w.update(drop=int(m[1]), away=int(m[2]), bmax=int(m[3]),
                         bnow=int(m[4]), flush=int(m[5]))
                cur["w"].append(w)
                cur["pend"] = None
                continue
            if R_BAD.search(line):
                cur["bad"] += 1


def judge(b, min_windows):
    start = next((w["t"] for w in b["w"] if w["out"] > 0), None)
    if start is None:
        return None, "no audio"
    ws = [w for w in b["w"] if w["t"] >= start + 60]
    n = len(ws)
    if n < min_windows:
        return None, f"{n} windows (< {min_windows})"
    res = {}
    # A1
    drops = [i for i, w in enumerate(ws) if w["drop"]]
    unexplained = []
    for i in drops:
        w = ws[i]
        recovered = any(x["bnow"] < DEADBAND for x in ws[i:i + 3])
        if w["away"] <= SLACK_US or not recovered:
            unexplained.append(round(w["t"]))
    res["A1"] = (len(drops) <= 0.01 * n and not unexplained,
                 f"drop windows {len(drops)}/{n}, unexplained {unexplained}")
    # A2
    ovf_up = sum(1 for a, c in zip(ws, ws[1:]) if c["ovf"] > a["ovf"])
    res["A2"] = (ovf_up <= 0.01 * n,
                 f"I2Sovf rose in {ovf_up}/{n - 1} windows "
                 f"({ws[0]['ovf']} -> {ws[-1]['ovf']})")
    # A3
    far = [round(w["t"]) for w in ws if abs(w["ppm"]) > 3000]
    loose = [round(w["t"]) for w in ws if w["bnow"] < DEADBAND and abs(w["ppm"]) > 500]
    res["A3"] = (not far and not loose and b["bad"] == 0,
                 f">3000 at {far[:5]}, >500 settled at {loose[:5]}, rail/relock {b['bad']}, "
                 f"ppm {min(w['ppm'] for w in ws)}..{max(w['ppm'] for w in ws)}")
    # A4 - 6-window (60 s) sums
    off = []
    for i in range(0, n - 5, 6):
        g = ws[i:i + 6]
        if any(not w["usb"] for w in g):
            continue
        o = sum(w["out"] for w in g); u = sum(w["usb"] for w in g)
        if abs(o - u) > 0.002 * u:
            off.append((round(g[0]["t"]), round((o - u) / u * 1e2, 2)))
    res["A4"] = (not off, f"60 s sums off >0.2 %: {off[:4]} (t, %)")
    # A5
    fl = [round(w["t"]) for w in ws if w["flush"]]
    bmax = max(w["bmax"] for w in ws)
    res["A5"] = (bmax < FLUSH_AT and len(fl) <= ovf_up, f"flush after 60 s at {fl[:5]}, backlog max {bmax}")
    return res, f"{n} windows, {ws[0]['t']:.0f}-{ws[-1]['t']:.0f} s"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--boot", help="only boots whose version contains this")
    ap.add_argument("--last", type=int, default=3, help="last N boots")
    ap.add_argument("--min-windows", type=int, default=6)
    ap.add_argument("--from", dest="t_from", type=float, default=0, help="uptime s")
    ap.add_argument("--to", dest="t_to", type=float, default=1e12, help="uptime s")
    a = ap.parse_args()
    bs = [b for b in boots(a.capture) if not a.boot or a.boot in b["ver"]]
    bs = bs[-a.last:]
    rc = 0
    for b in bs:
        if not b["w"]:
            print(f"{b['ver']}: no fields (firmware older than 16dad59b?)")
            continue
        b["w"] = [w for w in b["w"] if a.t_from <= w["t"] <= a.t_to] if (a.t_from or a.t_to < 1e12) else b["w"]
        res, info = judge(b, a.min_windows)
        if res is None:
            print(f"{b['ver']}: {info}")
            continue
        ok = all(v[0] for v in res.values())
        rc |= 0 if ok else 1
        print(f"{b['ver']}: {'PASS' if ok else 'FAIL'}  ({info})")
        for k, (p, txt) in res.items():
            print(f"  {k} {'ok  ' if p else 'FAIL'} {txt}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
