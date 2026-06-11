#!/usr/bin/env python3
"""
QMX Panadapter screenshot decoder.

Listens on a serial port for base64-encoded framebuffer dumps from the
Tab5 panadapter, decodes them, and saves PNG files to ~/Downloads.

Usage:
    python tools/screenshot_decode.py COM3

Stop esp_idf_monitor first (Ctrl+T, Ctrl+X). Long-press top-left 80x80
of the panadapter for 1 second to trigger a capture.
"""

import sys
import os
import re
import base64
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    print("pyserial not installed. Run: pip install pyserial pillow", file=sys.stderr)
    sys.exit(1)

try:
    from PIL import Image
except ImportError:
    print("Pillow not installed. Run: pip install pyserial pillow", file=sys.stderr)
    sys.exit(1)

BEGIN_RE = re.compile(rb"===SCREENSHOT_BEGIN w=(\d+) h=(\d+) fmt=(\w+) bytes=(\d+) raw=(\d+)===")
END_MARK = b"===SCREENSHOT_END==="
ABORT_MARK = b"===SCREENSHOT_ABORT==="
B64_LINE_RE = re.compile(rb"^[A-Za-z0-9+/=]+$")

# Save destination: %USERPROFILE%\Downloads on Windows, ~/Downloads elsewhere.
SAVE_DIR = Path(os.path.expanduser("~")) / "Downloads"


def packbits_decode(data: bytes, expected_len: int) -> bytes:
    """Decode PackBits-style RLE: 0..127 -> literal run of n+1 bytes,
    129..255 -> repeat next byte (257-n) times, 128 -> no-op."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n and len(out) < expected_len:
        ctrl = data[i]
        i += 1
        if ctrl <= 127:
            count = ctrl + 1
            out.extend(data[i:i + count])
            i += count
        elif ctrl >= 129:
            count = 257 - ctrl
            if i >= n:
                break
            out.extend(bytes([data[i]]) * count)
            i += 1
        # ctrl == 128: no-op
    return bytes(out)


def rgb565_to_rgb888(raw: bytes, w: int, h: int) -> bytes:
    out = bytearray(w * h * 3)
    for i in range(w * h):
        lo = raw[i * 2]
        hi = raw[i * 2 + 1]
        v = (hi << 8) | lo
        r5 = (v >> 11) & 0x1F
        g6 = (v >> 5) & 0x3F
        b5 = v & 0x1F
        out[i * 3 + 0] = (r5 << 3) | (r5 >> 2)
        out[i * 3 + 1] = (g6 << 2) | (g6 >> 4)
        out[i * 3 + 2] = (b5 << 3) | (b5 >> 2)
    return bytes(out)


def capture_one(ser):
    print("Waiting for screenshot... (long-press top-left of panadapter for 1 sec)")
    w = h = fmt = expected_bytes = raw_bytes = None
    b64_acc = bytearray()
    started = False

    while True:
        line = ser.readline()
        if not line:
            continue
        line = line.rstrip(b"\r\n")

        if not started:
            m = BEGIN_RE.search(line)
            if m:
                w = int(m.group(1))
                h = int(m.group(2))
                fmt = m.group(3).decode()
                expected_bytes = int(m.group(4))
                raw_bytes = int(m.group(5))
                print(f"BEGIN: {w}x{h} {fmt} {expected_bytes} bytes (raw {raw_bytes})")
                started = True
            continue

        if ABORT_MARK in line:
            print("Target aborted (out of memory?). Try again.")
            return False

        if END_MARK in line:
            print(f"END: received {len(b64_acc)} b64 chars")
            break

        if B64_LINE_RE.match(line):
            b64_acc.extend(line)
        elif b"timing" in line or b"chunk" in line:
            print(line.decode(errors="replace"), flush=True)

    payload = base64.b64decode(bytes(b64_acc))
    if len(payload) != expected_bytes:
        print(f"WARNING: decoded {len(payload)} bytes, expected {expected_bytes} "
              f"(UART loss, padding with zeros)",
              file=sys.stderr)
        if len(payload) < expected_bytes:
            payload = payload + b"\x00" * (expected_bytes - len(payload))
        else:
            payload = payload[:expected_bytes]

    if fmt == "rle565":
        raw = packbits_decode(payload, raw_bytes)
        if len(raw) != raw_bytes:
            print(f"WARNING: RLE decoded {len(raw)} bytes, expected {raw_bytes} "
                  f"(padding with zeros)", file=sys.stderr)
            if len(raw) < raw_bytes:
                raw = raw + b"\x00" * (raw_bytes - len(raw))
            else:
                raw = raw[:raw_bytes]
    elif fmt == "rgb565":
        raw = payload
    else:
        print(f"Unknown format: {fmt}", file=sys.stderr)
        return False

    rgb = rgb565_to_rgb888(raw, w, h)
    img = Image.frombytes("RGB", (w, h), rgb)

    # Tab5 runs sw_rotate=1 (LV_DISPLAY_ROTATION_90). If the saved PNG comes
    # out sideways relative to what you see on the screen, uncomment:
    # img = img.rotate(90, expand=True)

    SAVE_DIR.mkdir(parents=True, exist_ok=True)
    fname = SAVE_DIR / f"qmx_{datetime.now().strftime('%Y%m%d_%H%M%S')}.png"
    img.save(fname)
    print(f"Saved {fname}")
    return True


def main():
    if len(sys.argv) < 2:
        print(f"Usage: python {sys.argv[0]} <COMx> [baud]", file=sys.stderr)
        sys.exit(1)
    port = sys.argv[1]
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 921600

    print(f"Opening {port} @ {baud}...")
    print(f"Saving to: {SAVE_DIR}")
    ser = serial.Serial(port, baud, timeout=1)
    print("Connected. Ctrl+C to exit.")

    try:
        while True:
            capture_one(ser)
            print()
    except KeyboardInterrupt:
        print("\nBye.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()