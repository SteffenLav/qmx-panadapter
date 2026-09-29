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

*** WHY EACH REGION IS DECODED INDEPENDENTLY ***

A region's byte count is rarely a multiple of the 180-byte line, so its LAST
line is short (a 12000-sample region is 24000 bytes = 133 full lines + 60).
An earlier version of this script rebuilt one flat byte stream and zero-filled
every dropped line with 180 bytes - so dropping a short end-of-region line
shifted everything after it by 120 bytes, and the misaligned samples decoded
into fake silent runs and fake steps. That produced a spurious "one gap still
exits with a step of 563" against a true median of 1, and 74 silent runs where
the firmware had reported 45 gaps. Decoding per region gave 17 runs, every one
with a step of 1.

So: lines are collected PER REGION, each region is rebuilt on its own, its
expected line count is checked, and a region that is short or damaged is
reported and SKIPPED rather than decoded into plausible-looking nonsense.

Every line also carries a Fletcher-16 over its raw bytes. A corrupted line is
silenced at its correct length, never decoded - a corrupted-but-present line
otherwise becomes audio the click detector cannot tell from a real click,
which is exactly how a phantom "second source" was reported once already.
"""
import base64
import re
import sys
import wave

LINE_BYTES = 180          # input bytes per full base64 line (see rx_audio.c)


def fletcher16(data):
    s1 = s2 = 0
    for b in data:
        s1 = (s1 + b) % 255
        s2 = (s2 + s1) % 255
    return (s2 << 8) | s1


def parse(src):
    rate, want = 48000, 0
    gaps, regions = [], []
    begun = False
    for raw in open(src, encoding='utf-8', errors='replace'):
        ln = re.sub(r'\x1b\[[0-9;]*m', '', raw)

        m = re.search(r'RXCAP-BEGIN rate=(\d+) samples=(\d+)', ln)
        if m:
            rate, want = int(m.group(1)), int(m.group(2))
            gaps, regions, begun = [], [], True      # a later run supersedes
            continue

        m = re.search(r'RXCAP-GAP (\d+)', ln)
        if m and begun:
            gaps.append(int(m.group(1)))
            continue

        m = re.search(r'RXCAP-REGION (\d+) (\d+)', ln)
        if m and begun:
            regions.append({'start': int(m.group(1)),
                            'count': int(m.group(2)),
                            'lines': []})
            continue

        m = re.search(r'RXCAP (\d+) ([0-9a-f]{4}) ([A-Za-z0-9+/=]+)\s*$', ln)
        if m and begun and regions:
            regions[-1]['lines'].append(
                (int(m.group(1)), m.group(3), int(m.group(2), 16)))

    return rate, want, gaps, regions


def expected_lines(region):
    nb = region['count'] * 2
    return nb // LINE_BYTES + (1 if nb % LINE_BYTES else 0)


def rebuild(region):
    """Return (bytes, n_bad, ok). ok is False when the region is unusable."""
    nb = region['count'] * 2
    short = nb % LINE_BYTES
    exp = expected_lines(region)
    if len(region['lines']) != exp:
        return b'', 0, False

    buf = bytearray()
    n_bad = 0
    for k, (_idx, blob, want_ck) in enumerate(region['lines']):
        want_len = short if (short and k == exp - 1) else LINE_BYTES
        try:
            raw = base64.b64decode(blob)
        except Exception:
            raw = b''
        if len(raw) != want_len or fletcher16(raw) != want_ck:
            n_bad += 1
            raw = bytes(want_len)          # silence, at the CORRECT length
        buf += raw
    return bytes(buf[:nb]), n_bad, True


def main(src, dst):
    rate, want, gaps, regions = parse(src)
    if not regions:
        print('no RXCAP dump found in ' + src)
        return 1

    print('rate %d   capture was %d samples (%.1f s)' % (rate, want, want / rate))
    print('gaps recorded: %d   regions: %d' % (len(gaps), len(regions)))

    base = dst[:-4] if dst.lower().endswith('.wav') else dst
    manifest = []
    skipped = 0
    total_bad = 0

    for ri, region in enumerate(regions):
        data, n_bad, ok = rebuild(region)
        total_bad += n_bad
        if not ok:
            skipped += 1
            print('  region %2d  start %8d  SKIPPED - %d lines, expected %d '
                  '(truncated in the log; not decoded, rather than risk '
                  'misaligned samples)'
                  % (ri, region['start'], len(region['lines']),
                     expected_lines(region)))
            continue

        name = '%s.r%02d_s%d.wav' % (base, ri, region['start'])
        with wave.open(name, 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(data)
        inside = [g for g in gaps
                  if region['start'] <= g < region['start'] + region['count']]
        manifest.append((name, region['start'], region['count'], inside, n_bad))
        print('  region %2d  start %8d  %7.1f ms  gaps inside: %d  bad lines: %d  -> %s'
              % (ri, region['start'], region['count'] / rate * 1000.0,
                 len(inside), n_bad, name))

    print('')
    print('%d region WAVs written, %d skipped, %d corrupted line(s) silenced'
          % (len(manifest), skipped, total_bad))
    if total_bad or skipped:
        print('DO NOT treat silenced or skipped audio as a measurement.')

    tab = chr(9)
    with open(dst + '.manifest.txt', 'w') as f:
        for name, start, count, inside, n_bad in manifest:
            rel = ','.join(str(g - start) for g in inside)
            f.write(tab.join([name, str(start), str(count), rel, str(n_bad)]) + chr(10))
    print('gap offsets in the manifest are RELATIVE TO EACH REGION.')
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
