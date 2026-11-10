#!/usr/bin/env python3
"""Find SSB voice with the Tab5's own spectrum. RECEIVE ONLY - only set_freq is sent.

Step the dial across a phone segment in 40 kHz steps at zoom x1 (48 kHz span,
1024 bins, 47 Hz/bin), record ~6 s of spectrum frames per step, and score
every 2.4 kHz window:
  occupancy  = share of its bins whose 90th percentile is >= floor + 8 dB
  modulation = mean (p90 - p10) of those bins, dB   (speech is syllabic: large;
               a carrier or a data signal is steady: small)
An SSB voice signal is WIDE and MODULATED. A carrier is narrow. Noise is neither.

    python ssb_scan.py --lo 14112000 --hi 14350000 --sideband USB
"""
import argparse, json, sys, time
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import web_audio_capture as W

HDR, N = 24, 1024
DB_PER_Q = 100.0 / 255.0


def u32(b, o): return int.from_bytes(b[o:o + 4], "little")


def cmd(host, obj):
    """W.cmd with one retry: closing a /ws without the handshake left the fd in
    the device's session list, and a spectrum frame was then written into the
    next HTTP response (BadStatusLine, seen once). The command itself ran."""
    for k in range(3):
        try: return W.cmd(host, obj)
        except Exception as e:
            print(f"   cmd retry ({type(e).__name__})", flush=True); time.sleep(0.7)
    raise RuntimeError("cmd failed 3 times")


def collect(gen, secs, prev_lo):
    """Spectrum frames for `secs` from ONE long-lived socket."""
    rows, lo, span, t0 = [], None, None, time.time()
    for op, p in gen:
        now = time.time() - t0
        if now > secs: break
        if op != 0x2 or len(p) < HDR + N or p[0] != 0x01: continue
        sp = u32(p, 10)
        if sp == 0 or now < 1.5: continue                 # let the retune settle
        # RAW FFT bins (see spec bin mapping in index.html): bin k sits at
        # (dial - if_offset) + k*48000/1024, k = -512..511, negative k stored
        # at k+1024. Header view_lo/span describe the SCREEN view, not the bins.
        # (Read as lo + i*hz it put FT8 24 kHz low - caught by the 14.074 control.)
        ifo = int.from_bytes(p[22:24], "little", signed=True)
        lo, span = u32(p, 2) - ifo - 24000, 48000
        b = p[HDR:HDR + N]
        rows.append(b[N // 2:] + b[:N // 2])
    return lo, span, rows


def pct(v, q):
    v = sorted(v); return v[min(len(v) - 1, int(q * len(v)))]


def analyse(lo, span, rows, sideband):
    """Frame-normalised: the whole spectrum breathes 5-6 dB frame to frame
    (measured on empty windows), so each frame is referred to its own median
    before anything is compared across time."""
    import numpy as np
    X = np.array([list(r) for r in rows], dtype=float) * DB_PER_Q      # dB, [T, N]
    T = len(rows); hz = span / N; win = int(round(2400 / hz))
    X = X - np.median(X, axis=1, keepdims=True)                        # per-frame reference
    L = 10 ** (X / 10)
    c = np.cumsum(np.concatenate([np.zeros((T, 1)), L], axis=1), axis=1)
    PW = 10 * np.log10((c[:, win:] - c[:, :-win]) / win)                # [T, N-win+1] window power
    p10, p50, p90 = np.percentile(PW, [10, 50, 90], axis=0)
    base = np.median(p50)                                               # a typical empty window
    b90 = np.percentile(X, 90, axis=0); bfloor = np.median(b90)
    dc = N // 2
    out = []
    for a in range(20, N - 20 - win, 2):
        if a <= dc + 20 and a + win >= dc - 20: continue                # the LO spike
        mod, strength = p90[a] - p10[a], p90[a] - base
        occ = float((b90[a:a + win] >= bfloor + 4).mean())
        if mod < 4 or strength < 3 or occ < 0.3: continue
        out.append((mod * occ, a, occ, mod, strength))
    out.sort(key=lambda t: -t[0])
    keep = []
    for t in out:
        if all(abs(t[1] - k[1]) * hz > 3000 for k in keep): keep.append(t)
    res = []
    for sc, a, occ, mod, strength in keep:
        occb = [i for i in range(a, a + win) if b90[i] >= bfloor + 4]
        f_lo, f_hi = lo + occb[0] * hz, lo + (occb[-1] + 1) * hz
        dial = (f_lo - 250) if sideband == "USB" else (f_hi + 250)
        dial = int(round(dial / 500.0)) * 500
        res.append(dict(dial=dial, lo=int(f_lo), hi=int(f_hi), occ=round(occ, 2),
                        mod_db=round(float(mod), 1), peak_db=round(float(strength), 1), frames=T))
    noise_mod = float(np.median(p90 - p10))
    return noise_mod, res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.210")
    ap.add_argument("--lo", type=int, required=True)
    ap.add_argument("--hi", type=int, required=True)
    ap.add_argument("--sideband", default="USB")
    ap.add_argument("--secs", type=float, default=7)
    ap.add_argument("--json")
    a = ap.parse_args()
    allc = []
    sock, pending = W.ws_open(a.host); sock.settimeout(5)
    gen = W.frames(sock, pending)
    prev_lo = None
    c = a.lo + 34000                    # capture is dial-36k .. dial+12k
    covered = []
    try:
        while len(covered) < 14:
            cmd(a.host, {"action": "set_freq", "hz": c})
            lo, span, rows = collect(gen, a.secs, prev_lo)
            if len(rows) < 20:
                print(f"{c}: only {len(rows)} frames - instrument not alive, stopping"); return 2
            prev_lo = lo
            covered.append((lo, lo + span))
            floor, res = analyse(lo, span, rows, a.sideband)
            print(f"dial {c/1e6:.3f}  view {lo/1e6:.4f}..{(lo+span)/1e6:.4f}  {len(rows)} frames  "
                  f"empty-window mod {floor:.1f} dB  {len(res)} candidate(s)", flush=True)
            for r in res[:6]:
                if a.lo <= r["dial"] <= a.hi:
                    print(f"    dial {r['dial']/1e6:.4f}  block {r['lo']/1e6:.4f}-{r['hi']/1e6:.4f}  "
                          f"occ {r['occ']}  mod {r['mod_db']} dB  peak +{r['peak_db']} dB", flush=True)
                    allc.append(r)
            if lo + span >= a.hi: break
            c = lo + span + 34000          # next capture starts 2 kHz inside this one
    finally:
        try:
            sock.sendall(bytes([0x88, 0x80, 0, 0, 0, 0])); time.sleep(0.5)   # a proper close
        except Exception: pass
        sock.close()
    allc.sort(key=lambda r: -(r["occ"] * r["mod_db"]))
    if a.json: json.dump(allc, open(a.json, "w"), indent=1)
    print("covered:", ", ".join(f"{l/1e6:.4f}-{h/1e6:.4f}" for l, h in covered))
    print(f"{len(allc)} windows listed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
