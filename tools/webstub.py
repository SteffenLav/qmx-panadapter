#!/usr/bin/env python3
"""Serve main/net/www/index.html with a FAKE /api/status, so a web-UI change
can be exercised in a browser with no Tab5 and no flash.

Why this exists: v1.17.0 shipped with the entire web status pane dead, because
every check that evening hit /api/status with curl and nothing ever loaded the
page. tools/check_web.py and check_webui.py both PASSED on that file - they
parse the JavaScript, they do not run it. This is the behaviour gate.

    python tools/webstub.py --state scratchpad/js8_tx.json [--port 8088]

The state file is re-read on every request, so editing it changes what the page
sees on its next 1 Hz poll - no restart. Anything it does not define falls back
to the defaults below, which are a JS8 screen sitting idle.

/ws is NOT implemented by default: the spectrum is a separate socket, and its
absence is the condition under which the status pane was found dead. Keep it
that way unless the change under test is ON that socket.

    python tools/webstub.py --ws-audio 700

serves a /ws that pushes nothing but AUDIO frames (type 0x03), carrying a tone
at the given pitch in mu-law at 12 kHz - the wire format main/net/webserver_ws.c
sends. That exercises the parts of browser audio a flash cannot reach any
faster: the frame header, the mu-law table, the AudioWorklet, its rate trim and
the button's own states. It does NOT prove anything about the radio, the
demodulator or the link.
"""
import argparse, base64, hashlib, json, math, os, socket, struct, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "main", "net", "www", "index.html")

DEFAULT = {
    "screen": "ft8", "freq_hz": 14078000, "mode": "USB", "band": "20m",
    "passband_hz": 2700, "pb_lo_hz": 300, "pb_hi_hz": 3000,
    "cap_lo_hz": 0, "cap_hi_hz": 24000, "view_lo_hz": 14074000,
    "view_hi_hz": 14082000, "span_hz": 8000, "n_bins": 1024, "zoom": 1,
    "zoom_steps": [1, 2, 4, 8], "db_lo": -120, "db_hi": -20,
    "rit_hz": 0, "if_offset_hz": 0, "if_cal_hz": 0, "cw_pitch_hz": 700,
    "cw_filter_mask": 0, "paused": False, "wspr_en": False,
    "signal_dbm": -95, "qmx_fw": "1_04_010", "qso_count": 0,
    "time_src": "GPS", "utc_epoch": 1760000000,
    "wifi": {"ssid": "stub", "rssi": -52},
    "battery": {"pct": 87, "charging": False},
    "ft8": {
        "st": "idle", "text": "RX: 0 decoded", "proto": "JS8",
        "cq_parity": 0, "pause": False, "web_r": "",
        "ftx_active": False, "ftx_sent": 0, "ftx_total": 0, "ftx_result": "",
    },
}

JS8 = {
    "mode": "js8", "lines": [], "row_msg": [], "senders": [], "count": 0,
}


def merged(path):
    s = json.loads(json.dumps(DEFAULT))
    if path and os.path.exists(path):
        over = json.load(open(path, encoding="utf-8"))
        for k, v in over.items():
            if isinstance(v, dict) and isinstance(s.get(k), dict):
                s[k].update(v)
            else:
                s[k] = v
    return s


WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def mulaw_encode(pcm):
    """The Sun g711.c reference, the same one main/util/webaudio_codec.c now
    follows - so a mismatch in the browser is the browser's, not the stub's."""
    seg_uend = (0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF)
    v = pcm >> 2
    if v < 0:
        v, mask = -v, 0x7F
    else:
        mask = 0xFF
    if v > 8159:
        v = 8159
    v += (0x84 >> 2)
    for seg in range(8):
        if v <= seg_uend[seg]:
            break
    else:
        return 0x7F ^ mask
    return ((seg << 4) | ((v >> (seg + 1)) & 0x0F)) ^ mask


def ws_frame(payload):
    """One unmasked binary frame. Server-to-client is never masked."""
    n = len(payload)
    if n <= 125:
        hdr = struct.pack("!BB", 0x82, n)
    elif n < 65536:
        hdr = struct.pack("!BBH", 0x82, 126, n)
    else:
        hdr = struct.pack("!BBQ", 0x82, 127, n)
    return hdr + payload


def ws_audio_push(conn, pitch_hz):
    """100 ms of tone per tick, exactly as ws_push_task does - including the
    sequence number, so a browser that is dropping frames can say so."""
    RATE = 12000
    seq = 0
    phase = 0.0
    step = 2.0 * math.pi * pitch_hz / RATE
    nxt = time.time()
    try:
        while True:
            nxt += 0.1
            n = RATE // 10
            body = bytearray(6 + n)
            body[0] = 0x03                      # WS_FRAME_TYPE_AUDIO
            body[1] = 0
            body[2] = seq & 0xFF
            body[3] = (seq >> 8) & 0xFF
            body[4] = RATE & 0xFF
            body[5] = (RATE >> 8) & 0xFF
            for i in range(n):
                body[6 + i] = mulaw_encode(int(12000 * math.sin(phase)))
                phase += step
            seq = (seq + 1) & 0xFFFF
            conn.sendall(ws_frame(bytes(body)))
            d = nxt - time.time()
            if d > 0:
                time.sleep(d)
            else:
                nxt = time.time()
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass
    finally:
        try:
            conn.close()
        except Exception:
            pass


class H(BaseHTTPRequestHandler):
    state_path = None
    ws_audio_hz = 0

    def _send(self, body, ctype):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path.split("?", 1)[0]
        if p == "/ws":
            if not self.ws_audio_hz:
                self.send_error(404)
                return
            key = self.headers.get("Sec-WebSocket-Key")
            if not key:
                self.send_error(400)
                return
            acc = base64.b64encode(
                hashlib.sha1(key.encode("ascii") + WS_GUID).digest()).decode()
            self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
            self.wfile.flush()
            sys.stderr.write("ws: client up, pushing a %d Hz tone\n" % self.ws_audio_hz)
            self.close_connection = True
            ws_audio_push(self.connection, self.ws_audio_hz)
            return
        if p in ("/", "/index.html"):
            return self._send(open(PAGE, encoding="utf-8").read(), "text/html")
        if p == "/api/status":
            return self._send(json.dumps(merged(self.state_path)), "application/json")
        if p == "/api/js8":
            s = merged(self.state_path)
            j = dict(JS8)
            j.update(s.get("js8", {}))
            return self._send(json.dumps(j), "application/json")
        self._send("{}", "application/json")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            req = json.loads(raw or b"{}")
        except Exception:
            req = {}
        sys.stderr.write("POST %s %s\n" % (self.path, req))
        # js8_text_plan is the only POST whose ANSWER the page renders.
        if req.get("action") == "js8_text_plan":
            t = " ".join(str(req.get("text", "")).upper().split())
            frames = max(1, (len(t) + 12) // 13)
            return self._send(json.dumps({"ok": True, "frames": frames,
                                          "secs": frames * 15, "text": t}),
                              "application/json")
        self._send(json.dumps({"ok": True}), "application/json")

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default=None)
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--ws-audio", type=int, default=0, metavar="HZ",
                    help="serve /ws pushing an audio tone at HZ (0 = no /ws)")
    a = ap.parse_args()
    H.state_path = a.state
    H.ws_audio_hz = a.ws_audio
    print("stub on http://127.0.0.1:%d  page=%s  state=%s" % (a.port, PAGE, a.state))
    ThreadingHTTPServer(("127.0.0.1", a.port), H).serve_forever()
