#!/usr/bin/env python3
"""T188: per-band frequency and mode memory, through the web API. RECEIVE ONLY.

Walks 20 m <-> 40 m with the band button's own action (set_band) and checks the
frequency and mode the Tab5 lands on. Mode changes are spaced 5 s apart: nine in
15 s once took the QMX off the bus. Restores screen, frequency and mode at the end.
"""
import json, sys, time, urllib.request

H = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.210"
fails = 0


def st():
    return json.loads(urllib.request.urlopen(f"http://{H}/api/status", timeout=5).read())


def cmd(o):
    r = urllib.request.Request(f"http://{H}/api/cmd", data=json.dumps(o).encode(),
                               headers={"Content-Type": "application/json"})
    return urllib.request.urlopen(r, timeout=5).read().decode()[:100]


def settle(sec=5.0):
    time.sleep(sec)
    s = st()
    return s["freq_hz"], s["mode"]


def check(label, got, want_f, want_m):
    global fails
    ok = (want_f is None or got[0] == want_f) and got[1] == want_m
    if not ok: fails += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {label}: {got[0]} {got[1]}  (want {want_f if want_f else 'any'} {want_m})")


before = st()
print("before:", before["freq_hz"], before["mode"], before["screen"])
bands = before["bands"]
c20 = next(b["center_hz"] if isinstance(b, dict) else b for b in bands
           if 14000000 <= (b["center_hz"] if isinstance(b, dict) else b) <= 14350000)
c40 = next(b["center_hz"] if isinstance(b, dict) else b for b in bands
           if 7000000 <= (b["center_hz"] if isinstance(b, dict) else b) <= 7300000)
print("band centres:", c20, c40)
try:
    if before["screen"] != "panadapter":
        cmd({"action": "set_screen", "screen": "pan"}); time.sleep(4)
    cmd({"action": "set_band", "hz": c20}); settle()
    cmd({"action": "set_freq", "hz": 14200000}); time.sleep(1)
    cmd({"action": "set_mode", "mode": "USB"}); got = settle()
    check("20 m, set by hand", got, 14200000, "USB")

    cmd({"action": "set_band", "hz": c40}); got = settle()
    stored40 = got
    print(f"  40 m after the band button: {got[0]} {got[1]}")
    if got[1] != "LSB":
        # a mode may already be stored for 40 m from before this test; say so, do not guess
        print("  (40 m did not come up LSB - a mode is stored for it, or the rule failed; see the capture)")
    cmd({"action": "set_freq", "hz": 7150000}); time.sleep(1)
    cmd({"action": "set_mode", "mode": "LSB"}); got = settle()
    check("40 m, set by hand", got, 7150000, "LSB")

    cmd({"action": "set_band", "hz": c20}); got = settle()
    check("back to 20 m: recalled", got, 14200000, "USB")
    cmd({"action": "set_band", "hz": c40}); got = settle()
    check("back to 40 m: recalled", got, 7150000, "LSB")

    cmd({"action": "set_mode", "mode": "AM"}); got = settle()
    check("40 m, AM chosen while staying", got, 7150000, "AM")
    cmd({"action": "set_band", "hz": c20}); got = settle()
    check("20 m again: its own mode, not 40 m's AM", got, 14200000, "USB")
    cmd({"action": "set_band", "hz": c40}); got = settle()
    check("40 m again: AM remembered", got, 7150000, "AM")
    cmd({"action": "set_mode", "mode": "LSB"}); got = settle()
    check("40 m put back to LSB", got, 7150000, "LSB")
finally:
    cmd({"action": "set_freq", "hz": before["freq_hz"]}); time.sleep(1)
    cmd({"action": "set_mode", "mode": before["mode"]}); time.sleep(2)
    if before["screen"] != "panadapter":
        cmd({"action": "set_screen", "screen": before["screen"]}); time.sleep(3)
    s = st(); print("restored:", s["freq_hz"], s["mode"], s["screen"])
print("band_mem_test:", "FAILED %d" % fails if fails else "all checks passed")
sys.exit(1 if fails else 0)
