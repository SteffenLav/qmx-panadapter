#!/usr/bin/env python3
"""Find keyed CW signals with the Tab5's own spectrum. RECEIVE ONLY - only set_freq is sent.

One dial position covers dial-36 kHz .. dial+12 kHz (see ssb_scan.py for the
bin mapping). A CW signal is NARROW and KEYED: one or two 47 Hz bins whose 90th
percentile stands well over the floor, with a large p90-p10 swing, and nothing
like it 150 Hz either side.

    python tools/cw_scan.py --lo 7000000 --hi 7040000
"""
import argparse, sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import web_audio_capture as W
import ssb_scan as S


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.210")
    ap.add_argument("--lo", type=int, required=True)
    ap.add_argument("--hi", type=int, required=True)
    ap.add_argument("--secs", type=float, default=9)
    a = ap.parse_args()
    import numpy as np
    sock, pending = W.ws_open(a.host); sock.settimeout(5)
    gen = W.frames(sock, pending)
    try:
        S.cmd(a.host, {"action": "set_freq", "hz": a.lo + 34000})
        lo, span, rows = S.collect(gen, a.secs, None)
    finally:
        try: sock.sendall(bytes([0x88, 0x80, 0, 0, 0, 0])); time.sleep(0.5)
        except Exception: pass
        sock.close()
    if len(rows) < 20:
        print(f"only {len(rows)} frames - instrument not alive"); return 2
    X = np.array([list(r) for r in rows], dtype=float) * S.DB_PER_Q
    X = X - np.median(X, axis=1, keepdims=True)          # each frame against its own median
    p10, p90 = np.percentile(X, [10, 90], axis=0)
    floor = float(np.median(p90)); hz = span / S.N; dc = S.N // 2
    print(f"view {lo/1e6:.4f}..{(lo+span)/1e6:.4f}, {len(rows)} frames, floor p90 {floor:.1f} dB, "
          f"typical p90-p10 {float(np.median(p90 - p10)):.1f} dB")
    out = []
    for k in range(8, S.N - 8):
        if abs(k - dc) < 12: continue                     # the LO spike
        f = lo + (k + 0.5) * hz
        if not (a.lo <= f <= a.hi): continue
        if p90[k] < floor + 10 or p90[k] - p10[k] < 8: continue
        if p90[k] < max(p90[k - 1], p90[k + 1]): continue
        side = max(p90[k - 6:k - 2].max(), p90[k + 3:k + 7].max())
        if p90[k] - side < 6: continue
        out.append((float(p90[k] - floor), int(round(f / 10.0)) * 10, float(p90[k] - p10[k])))
    out.sort(reverse=True)
    for s, f, sw in out[:12]:
        print(f"  {f/1e6:.5f} MHz  +{s:.1f} dB over the floor, keying swing {sw:.1f} dB")
    print(f"{len(out)} keyed narrow signal(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
