#!/usr/bin/env python3
"""Fill the Tab5's WiFi co-processor RX queue with full-size frames.

WHY: Bryan N0LUF's hosted link dies, and his log says the drain's
"absurd delta" guard (16 x ESP_RX_BUFFER_SIZE = 24,576 B) is skipping a read
of 25,942 B - seventeen full-size Ethernet frames (1514 + 12 B header) - and
advancing the host counter anyway. That manufactures a desync the link never
recovers from. Four for four in his log, each one the last oversize event
before the link died.

This fills the slave's queue the same way his network does: unicast UDP at
full MTU, as fast as the wire takes it. Unicast rather than broadcast so
nothing else on the LAN is disturbed.

Usage: sdio_flood.py <ip> [seconds] [payload] [rate_pps]  (0 rate = flat out)
"""
import socket, sys, time

ip      = sys.argv[1]
secs    = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
payload = int(sys.argv[3])   if len(sys.argv) > 3 else 1472   # -> 1514 on the wire
rate    = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
buf = bytes(payload)
port = 9                     # discard
t0 = time.time()
n = 0
gap = (1.0 / rate) if rate > 0 else 0.0
nxt = t0
while time.time() - t0 < secs:
    try:
        s.sendto(buf, (ip, port))
        n += 1
    except OSError:
        time.sleep(0.001)
    if gap:
        nxt += gap
        d = nxt - time.time()
        if d > 0: time.sleep(d)
el = time.time() - t0
print(f"sent {n} frames of {payload} B in {el:.1f}s "
      f"= {n/el:,.0f} pps, {n*(payload+42)*8/el/1e6:.1f} Mbit/s on the wire")
