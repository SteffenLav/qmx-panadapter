#!/usr/bin/env python3
"""Decode an RXCAP serial dump back into per-region WAVs plus a gap index.

The recorder in main/audio/rx_audio.c base64s its PSRAM capture straight into
the ESP_LOG stream, because on the rx-audio track WiFi wedges until a reboot
and a reboot clears the buffer - so a WiFi fetch of a WiFi-off recording is
impossible in principle. The serial capture needs no network and survives.

The dump is BAUD-LIMITED (measured 11.5 KB/s = 115200), so the device sends
only the diagnostically interesting audio: one reference region of ordinary
background, plus a window either side of every gap. Each region carries its
own start sample index, and overlapping windows are merged on the device.

    python tools/rxcap_decode.py scratchpad/capture-dev.txt out.wav

Writes out.r00_s<start>.wav ... and out.wav.manifest.txt. Gap offsets in the
manifest are RELATIVE TO EACH REGION.

Lines are INDEXED, so a dropped line is detected rather than silently
shifting every later sample. Missing chunks are filled with silence and
reported loudly - never quietly closed over.
"""
import base64
import re
import sys
import wave

LINE_BYTES = 180          # input bytes per emitted base64 line (see rx_audio.c)


def main(src, dst):
    rate, want = 48000, 0
    lines, gaps, regions = {}, [], []
    begun = False

    with open(src, encoding='utf-8', errors='replace') as f:
        for raw in f:
            ln = re.sub(r'\x1b\[[0-9;]*m', '', raw)

            m = re.search(r'RXCAP-BEGIN rate=(\d+) samples=(\d+)', ln)
            if m:
                rate, want = int(m.group(1)), int(m.group(2))
                lines, gaps, regions, begun = {}, [], [], True   # later run wins
                continue

            m = re.search(r'RXCAP-GAP (\d+)', ln)
            if m and begun:
                gaps.append(int(m.group(1)))
                continue

            m = re.search(r'RXCAP-REGION (\d+) (\d+)', ln)
            if m and begun:
                regions.append([int(m.group(1)), int(m.group(2))])
                continue

            m = re.search(r'RXCAP (\d+) ([0-9a-f]{4}) ([A-Za-z0-9+/=]+)\s*$', ln)
            if m and begun:
                lines[int(m.group(1))] = (m.group(3), int(m.group(2), 16))

    if not begun or not lines:
        print('no RXCAP dump found in ' + src)
        return 1

    # Verify each line's Fletcher-16 before trusting it. A corrupted-but-present
    # line decodes into plausible-looking audio and reads as a CLICK - the
    # instrument manufacturing the artefact it measures. Drop those; a missing
    # measurement is honest, a wrong one is not.
    corrupt = []
    for i in sorted(lines):
        blob, want_ck = lines[i]
        try:
            raw = base64.b64decode(blob)
        except Exception:
            corrupt.append(i)
            continue
        s1 = s2 = 0
        for b in raw:
            s1 = (s1 + b) % 255
            s2 = (s2 + s1) % 255
        if ((s2 << 8) | s1) != want_ck:
            corrupt.append(i)
    for i in corrupt:
        del lines[i]

    n = max(lines) + 1
    missing = [i for i in range(n) if i not in lines]
    data = bytearray()
    for i in range(n):
        data += base64.b64decode(lines[i][0]) if i in lines else bytes(LINE_BYTES)
    if corrupt:
        print('CHECKSUM FAILURES: %d line(s) dropped -> %s%s'
              % (len(corrupt), corrupt[:10], '...' if len(corrupt) > 10 else ''))

    print('rate %d   capture was %d samples (%.1f s)' % (rate, want, want / rate))
    print('lines %d/%d   MISSING %d%s' % (
        len(lines), n, len(missing),
        ('  -> ' + str(missing[:10]) + ('...' if len(missing) > 10 else '')) if missing else ''))
    if missing:
        print('  WARNING: %d samples filled with SILENCE - do not treat those as measurements'
              % (len(missing) * LINE_BYTES // 2))
    print('gaps recorded: %d   regions: %d' % (len(gaps), len(regions)))

    # Regions appear in the line stream contiguously, in emission order.
    off = 0
    manifest = []
    base = dst[:-4] if dst.lower().endswith('.wav') else dst
    for ri, (start, count) in enumerate(regions):
        nb = count * 2
        seg = bytes(data[off:off + nb])
        off += nb
        name = '%s.r%02d_s%d.wav' % (base, ri, start)
        with wave.open(name, 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(seg)
        inside = [g for g in gaps if start <= g < start + count]
        manifest.append((name, start, count, inside))
        print('  region %2d  start %8d  %7.1f ms  gaps inside: %d  -> %s'
              % (ri, start, count / rate * 1000.0, len(inside), name))

    tab = chr(9)
    with open(dst + '.manifest.txt', 'w') as f:
        for name, start, count, inside in manifest:
            rel = ','.join(str(g - start) for g in inside)
            f.write(tab.join([name, str(start), str(count), rel]) + chr(10))

    print('wrote %d region WAVs and %s.manifest.txt' % (len(manifest), dst))
    print('gap offsets in the manifest are RELATIVE TO EACH REGION.')
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
