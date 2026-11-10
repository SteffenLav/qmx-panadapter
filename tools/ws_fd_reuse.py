"""Does a spectrum frame land in an HTTP response after a WS client leaves?
RX only: opens /ws, reads a few frames, drops the socket WITHOUT a close frame
(what a page reload does), then at once does GET /api/status on new sockets and
checks the first bytes of each reply. Changes no radio state."""
import socket, sys, time
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import web_audio_capture as W
host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.210"
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 40
bad = ok = err = 0
for r in range(rounds):
    s, pend = W.ws_open(host); s.settimeout(3)
    g = W.frames(s, pend); n = 0
    for op, p in g:
        n += 1
        if n >= 3: break
    s.close()                                  # no close frame
    for k in range(3):
        try:
            c = socket.create_connection((host, 80), timeout=4)
            c.sendall(b"GET /api/status HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            c.settimeout(4); first = c.recv(16); c.close()
            if first.startswith(b"HTTP/1."): ok += 1
            else: bad += 1; print(f"round {r} req {k}: reply starts {first[:8].hex()}", flush=True)
        except Exception as e:
            err += 1
    time.sleep(0.3)
print(f"{rounds} rounds: {ok} clean replies, {bad} CORRUPTED (not starting 'HTTP/1.'), {err} socket errors")
