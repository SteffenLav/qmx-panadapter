#!/usr/bin/env python3
"""Cut kiwirecorder output into JS8 slot WAVs in the corpus format.

    # 1. record (from the kiwiclient checkout, NOT with python -I: it needs
    #    png.py and mod_pywebsocket beside it)
    python kiwirecorder.py -s <host> -p <port> -f 7078 -m usb \
        -L 200 -H 3000 --resample 12000 --dt-sec 15 -d <rawdir>

    # 2. cut
    python tools/js8_kiwi_slots.py <rawdir> --out test/wav_reference_js8_kiwi

WHY THIS EXISTS. The on-air corpus can only hold what this antenna hears, and
7.078 here is the heartbeat network - across ~90 slots on 2026-10-09 not one
free-text (data) frame was heard, so J7's decoder has no real input to be
measured against. A public KiwiSDR anywhere in the world gives slots from a
band that IS carrying JS8 chat, at our exact corpus shape, with no dependency
on the bench radio.

WHAT THIS IS NOT. There is no .txt beside these files, because there is no
board decode to record. Generate one with the harness itself once a slot is
known good:

    ./js8_wav_harness <outdir>

and only then is the directory a regression floor. Until then it is input, not
a baseline. See test/wav_reference_js8/README.md for why the floor is written
down at all.

⚠ THE SLOT GRID IS THE RECORDER'S LOCAL CLOCK, NOT THE SIGNAL. kiwirecorder's
--dt-sec cuts when the sec-of-day block changes at the moment the buffer is
written, so network and buffer latency push every cut late by a fixed amount
(typically under a second). That shows up as a constant DT in every decode,
not as a lost decode. MEASURE it from the DT column of the first decodes, then
pass --shift-ms to take it out. Do not guess the number.
"""

import argparse
import calendar
import os
import re
import struct
import sys
import wave

SR_HZ = 12000          # what js8_wav_harness demands: 12 kHz mono 16-bit

# 20261009T134500Z_7078000_usb.wav
NAME_RE = re.compile(r"^(\d{8}T\d{6}Z)_")


def parse_start_utc(path):
    m = NAME_RE.match(os.path.basename(path))
    if not m:
        return None
    t = m.group(1)
    st = (int(t[0:4]), int(t[4:6]), int(t[6:8]),
          int(t[9:11]), int(t[11:13]), int(t[13:15]), 0, 0, 0)
    return calendar.timegm(st)


def read_pcm16(path):
    """Return (samples_as_bytes, rate). Raises on anything the harness rejects."""
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1:
            raise ValueError("%d channels - record mono" % w.getnchannels())
        if w.getsampwidth() != 2:
            raise ValueError("%d-bit - the corpus is 16-bit" % (8 * w.getsampwidth()))
        rate = w.getframerate()
        return w.readframes(w.getnframes()), rate


def write_slot(path, frames):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR_HZ)
        w.writeframes(frames)


def normalise(frames, target):
    """Scale to `target` of full scale, and report the pre-scale peak.

    The board's own slot WAVs are normalised, and the harness divides by 32768
    to reach the +/-1.0 convention monitor_process() needs. An un-normalised
    slot recorded off a quiet receiver sits 40 dB down and finds no candidates.
    The returned peak is the KiwiSDR equivalent of /api/slot.json peak_float.
    """
    n = len(frames) // 2
    vals = struct.unpack("<%dh" % n, frames[:2 * n])
    peak = max((abs(v) for v in vals), default=0)
    if peak == 0:
        return frames, 0.0
    g = (target * 32767.0) / peak
    out = struct.pack("<%dh" % n,
                      *(max(-32768, min(32767, int(round(v * g)))) for v in vals))
    return out, peak / 32768.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("indir", help="directory kiwirecorder wrote into")
    ap.add_argument("--out", required=True, help="where the slot WAVs go")
    ap.add_argument("--period", type=float, default=15.0,
                    help="slot length in seconds (JS8 Normal = 15, Slow = 30)")
    ap.add_argument("--shift-ms", type=int, default=0,
                    help="take each slot this many ms LATER in the stream; "
                         "measure it from the DT column, never guess it")
    ap.add_argument("--min-peak", type=float, default=0.0,
                    help="skip slots whose pre-normalisation peak is below this "
                         "(0 keeps everything, including the silent slots that "
                         "catch a decoder inventing decodes)")
    ap.add_argument("--norm", type=float, default=0.98,
                    help="normalisation target as a fraction of full scale")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    slot_n = int(round(args.period * SR_HZ))
    shift = int(round(args.shift_ms / 1000.0 * SR_HZ))

    files = sorted(f for f in os.listdir(args.indir) if f.lower().endswith(".wav"))
    if not files:
        print("no .wav in %s" % args.indir, file=sys.stderr)
        return 1

    written = skipped = 0
    for fn in files:
        path = os.path.join(args.indir, fn)
        start = parse_start_utc(path)
        if start is None:
            print("  %s: name is not kiwirecorder's YYYYMMDDTHHMMSSZ_ form - "
                  "skipped" % fn, file=sys.stderr)
            continue
        try:
            pcm, rate = read_pcm16(path)
        except Exception as e:
            print("  %s: %s" % (fn, e), file=sys.stderr)
            continue
        if rate != SR_HZ:
            print("  %s: %d Hz - record with --resample %d" % (fn, rate, SR_HZ),
                  file=sys.stderr)
            continue

        total = len(pcm) // 2
        # First slot boundary at or after this file's start, on the UTC grid.
        per = args.period
        first = (int(start / per) + (0 if start % per == 0 else 1)) * per
        k = 0
        while True:
            off = int(round((first + k * per - start) * SR_HZ)) + shift
            k += 1
            if off < 0:
                continue
            if off + slot_n > total:
                break
            frames = pcm[2 * off: 2 * (off + slot_n)]
            frames, peak = normalise(frames, args.norm)
            if peak < args.min_peak:
                skipped += 1
                continue
            utc = int(first + (k - 1) * per)
            import time as _t
            name = _t.strftime("%y%m%d_%H%M%S", _t.gmtime(utc)) + ".wav"
            write_slot(os.path.join(args.out, name), frames)
            written += 1

    print("wrote %d slot(s) to %s%s" %
          (written, args.out,
           (", skipped %d below --min-peak" % skipped) if skipped else ""))
    # Plain ASCII: a Windows console here is cp1252 and a warning sign raises
    # UnicodeEncodeError, which would lose the whole message.
    print("WARNING: no .txt written - these have no board decode. Run "
          "./js8_wav_harness %s to see what they hold." % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
