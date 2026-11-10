#!/usr/bin/env python3
"""Record the Tab5's web audio stream (0x03 mu-law frames on /ws) to WAV.

RECEIVE ONLY. Nothing here keys the radio. Used for T18 (web noise reduction)
to get real QMX audio - after the Tab5's mode filter, AGC and limiter, which is
exactly what the browser NR sees - into a host harness.

    python tools/web_audio_capture.py --host 192.168.1.210 --out cap.wav --secs 30
    python tools/web_audio_capture.py --host ... --tune 14250000 --mode USB ...

--tune/--mode switch the Tab5 to the panadapter first. --restore puts the
screen/frequency/mode back afterwards. Stdlib only.
"""
import argparse, base64, json, os, socket, struct, sys, time, urllib.request, wave

AUDIO = 0x03
HDR = 6


def cmd(host, obj):
    req = urllib.request.Request(f"http://{host}/api/cmd", data=json.dumps(obj).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read() or b"{}")


def status(host):
    with urllib.request.urlopen(f"http://{host}/api/status", timeout=5) as r:
        return json.loads(r.read())


def ws_open(host):
    s = socket.create_connection((host, 80), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    if b" 101 " not in buf.split(b"\r\n")[0]:
        raise RuntimeError(buf[:200])
    return s, buf.split(b"\r\n\r\n", 1)[1]


def frames(s, pending):
    buf = pending
    def need(n):
        nonlocal buf
        while len(buf) < n:
            chunk = s.recv(65536)
            if not chunk:
                raise EOFError
            buf += chunk
    while True:
        need(2)
        op, ln = buf[0] & 0x0F, buf[1] & 0x7F
        off = 2
        if ln == 126:
            need(4); ln = struct.unpack(">H", buf[2:4])[0]; off = 4
        elif ln == 127:
            need(10); ln = struct.unpack(">Q", buf[2:10])[0]; off = 10
        need(off + ln)
        payload, buf = buf[off:off + ln], buf[off + ln:]
        yield op, payload


def ulaw_decode(b):
    u = (~b) & 0xFF
    sign, exp, man = u & 0x80, (u >> 4) & 7, u & 0x0F
    v = (((man << 3) + 0x84) << exp) - 0x84
    return -v if sign else v


ULAW = [ulaw_decode(i) for i in range(256)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.210")
    ap.add_argument("--out", required=True)
    ap.add_argument("--secs", type=float, default=30)
    ap.add_argument("--tune", type=int)
    ap.add_argument("--mode")
    ap.add_argument("--restore", action="store_true")
    a = ap.parse_args()

    before = status(a.host)
    if a.tune or a.mode:
        if before.get("screen") != "panadapter":
            cmd(a.host, {"action": "set_screen", "screen": "pan"}); time.sleep(2.0)
        if a.tune:
            cmd(a.host, {"action": "set_freq", "hz": a.tune}); time.sleep(0.6)
        if a.mode:
            cmd(a.host, {"action": "set_mode", "mode": a.mode}); time.sleep(1.5)
    st = status(a.host)
    print(f"dial {st.get('freq_hz')} mode {st.get('mode')} screen {st.get('screen')}", flush=True)

    s, pending = ws_open(a.host)
    s.settimeout(5)
    r = cmd(a.host, {"action": "web_audio", "on": True})
    print("web_audio:", r, flush=True)
    pcm, seq_prev, gaps, nfr, rate = bytearray(), None, 0, 0, 12000
    t0 = time.time()
    try:
        for op, p in frames(s, pending):
            if op == 0x8:
                print("server closed"); break
            if op != 0x2 or not p or p[0] != AUDIO or len(p) <= HDR:
                if time.time() - t0 > a.secs: break
                continue
            seq = p[2] | (p[3] << 8)
            rate = (p[4] | (p[5] << 8)) or 12000
            if seq_prev is not None and seq != ((seq_prev + 1) & 0xFFFF):
                gaps += 1
            seq_prev = seq; nfr += 1
            for b in p[HDR:]:
                pcm += struct.pack("<h", ULAW[b])
            if time.time() - t0 > a.secs: break
    finally:
        try: cmd(a.host, {"action": "web_audio", "on": False})
        except Exception as e: print("web_audio off failed:", e)
        s.close()
    with wave.open(a.out, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(pcm))
    print(f"{nfr} frames, {gaps} seq gaps, {len(pcm)//2} samples @ {rate} Hz -> {a.out}")

    if a.restore:
        if before.get("freq_hz"):
            cmd(a.host, {"action": "set_freq", "hz": before["freq_hz"]}); time.sleep(0.6)
        if before.get("mode"):
            cmd(a.host, {"action": "set_mode", "mode": before["mode"]}); time.sleep(1.0)
        if before.get("screen") and before["screen"] != "panadapter":
            cmd(a.host, {"action": "set_screen", "screen": before["screen"]})
        time.sleep(2.0)
        st = status(a.host)
        print(f"restored: dial {st.get('freq_hz')} mode {st.get('mode')} screen {st.get('screen')}")


if __name__ == "__main__":
    main()
