#!/usr/bin/env python3
"""Record a JS8 (or FT8/FT4) reference corpus off a running board.

    python tools/js8_corpus.py 192.168.1.210 --out test/wav_reference_js8
    python tools/js8_corpus.py 192.168.1.210 --slots 40          # then stop
    python tools/js8_corpus.py 192.168.1.210 --off               # disarm only

WHY THIS AND NOT rxcap. /api/slot.wav serves the decoder's OWN slot buffer -
12 kHz, mono, slot-aligned to the UTC boundary by dsp_ft8_capture_begin()'s
backfill, which is the same shape as the 35 FT8 references in
test/wav_reference. /api/rxaudio.wav is the 48 kHz codec output after the AGC
and the audio filter chain; the decoder never hears it, so a corpus built from
it would measure the wrong signal.

WHAT THE .txt BESIDE EACH .wav IS, AND IS NOT. It holds what THIS BOARD
decoded for that slot, read from /api/decodes. That is a regression baseline:
it catches a change that makes the decoder worse than it is today. It is NOT
ground truth, because it is this implementation agreeing with itself - a slot
JS8Call would have decoded and we did not is recorded here as empty. Every
file written while the sub-mode is JS8 gets that stated in its header, so a
later reader cannot mistake one for the other.
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request


def get(base, path, timeout=20):
    with urllib.request.urlopen(base + path, timeout=timeout) as r:
        return r.read()


def get_json(base, path, timeout=20):
    return json.loads(get(base, path, timeout).decode("utf-8", "replace"))


def post_cmd(base, obj, timeout=20):
    req = urllib.request.Request(
        base + "/api/cmd",
        data=json.dumps(obj).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def slot_name(utc):
    t = time.gmtime(utc)
    return time.strftime("%y%m%d_%H%M%S", t)


def decodes_for_slot(base, utc, period_s):
    """Rows whose LAST decode falls inside the slot at `utc`.

    ⚠ /api/decodes is a per-STATION aggregate, not a per-slot decode list: a
    row carries `call`, `text`, `snr`, `hz`, `dt` and `age` (seconds since
    that station was last heard) and no absolute time at all. So the slot is
    reconstructed as `utc_epoch - age`, read from /api/status in the same
    breath, and a row is kept when that lands inside this slot's window.

    Two consequences worth stating, because neither is visible in the output:
    a station heard in this slot AND an earlier one appears once, with this
    slot's values; and a station heard in an earlier slot but not this one is
    correctly excluded, but only to within the 1 s resolution of `age`."""
    try:
        st = get_json(base, "/api/status", timeout=15)
        d = get_json(base, "/api/decodes", timeout=15)
    except Exception as e:                      # noqa: BLE001 - diagnostic only
        return None, None, "decodes unavailable: %s" % e
    now = st.get("utc_epoch")
    if not now:
        return None, None, "no utc_epoch in /api/status"
    freq_hz = st.get("freq_hz")
    out = []
    for r in d.get("rows") or []:
        if not isinstance(r, dict):
            continue
        age = r.get("age")
        if age is None:
            continue
        try:
            heard = int(now) - int(age)
        except (TypeError, ValueError):
            continue
        if utc <= heard < utc + period_s:
            out.append(r)
    return out, freq_hz, None


def write_txt(path, utc, proto, peak, fpeak, freq_hz, rows, note):
    """One line per decode, in the shape the .txt files in test/wav_reference
    use: HHMMSS  SNR  DT  FREQ ~  MESSAGE - so one comparator reads both
    corpora.

    The header carries what the filename cannot: the DIAL FREQUENCY, without
    which a corpus file cannot be told from one recorded on another band, and
    peak_float, the decoder's own input level before the WAV was normalised."""
    hhmmss = time.strftime("%H%M%S", time.gmtime(utc))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# %s slot UTC %d  dial %d Hz  peak_float %.2f  peak %d\n"
                % (proto, utc, freq_hz or 0, fpeak, peak))
        f.write("# %s\n" % note)
        if rows is None:
            f.write("# /api/decodes was unreachable - expected set UNKNOWN,\n")
            f.write("# not empty. Do not treat this file as a baseline.\n")
            return
        for r in rows:
            msg = (r.get("text") or "").strip()
            if not msg:
                continue
            snr = r.get("snr", 0)
            hz = r.get("hz", 0)
            # ⚠ `dt` is int16 MILLISECONDS here (ft8_screen.h:31, last_dt_ms),
            # while the FT8 .txt column is seconds. Printing it raw put "720.0"
            # in the first 18 files written on 2026-10-09 - a plausible-looking
            # number that was 1000x wrong, which is the worst kind.
            dt_ms = r.get("dt", 0)
            try:
                f.write("%s %3d %4.1f %4d ~  %s\n"
                        % (hhmmss, int(snr), float(dt_ms) / 1000.0,
                           int(hz), msg))
            except (TypeError, ValueError):
                f.write("%s   0  0.0    0 ~  %s\n" % (hhmmss, msg))


def say(msg):
    """Flushed, always. The first long run of this tool wrote four slots and
    then stopped, and because stdout was block-buffered the output file was
    empty - so "it is working" and "it is wedged" looked identical for five
    minutes. Progress that is not flushed is not progress."""
    print(msg, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--out", default="test/wav_reference_js8")
    ap.add_argument("--slots", type=int, default=0,
                    help="stop after this many NEW slots (0 = run until Ctrl-C)")
    ap.add_argument("--min-peak", type=int, default=1,
                    help="skip a slot whose peak sample is below this - a dead "
                         "capture is not corpus, it is a bug report")
    ap.add_argument("--off", action="store_true",
                    help="disarm the recorder and exit")
    a = ap.parse_args()

    base = a.host if a.host.startswith("http") else "http://" + a.host

    if a.off:
        print(post_cmd(base, {"action": "slotcap", "on": False}))
        return 0

    os.makedirs(a.out, exist_ok=True)
    r = post_cmd(base, {"action": "slotcap", "on": True})
    if not r.get("armed"):
        print("ERROR: the board did not arm: %s" % r, file=sys.stderr)
        return 1
    say("armed; writing to %s  (Ctrl-C to stop)" % a.out)

    seen = r.get("seq", 0)
    kept = 0
    try:
        while True:
            time.sleep(2.0)
            try:
                st = get_json(base, "/api/slot.json", timeout=15)
            except Exception as e:              # noqa: BLE001
                # Broad on purpose: a poll that dies on an unexpected exception
                # type stops the recording, and the perishable thing here is
                # the band, not the tidiness of the except clause.
                say("  poll failed (%s: %s) - retrying" % (type(e).__name__, e))
                continue
            if not st.get("armed"):
                print("ERROR: the board disarmed itself - check the log for "
                      "'slotcap: no PSRAM'", file=sys.stderr)
                return 1
            seq = st.get("seq", 0)
            if seq == seen or not st.get("held"):
                continue
            seen = seq
            utc = int(st["slot_utc"])
            peak = int(st.get("peak", 0))
            proto = st.get("proto", "?")
            if peak < a.min_peak:
                say("  skip slot %d: peak=%d (silent capture)" % (utc, peak))
                continue

            stem = os.path.join(a.out, slot_name(utc))
            try:
                wav = get(base, "/api/slot.wav", timeout=30)
            except Exception as e:              # noqa: BLE001
                say("  slot %d: download failed (%s: %s)"
                    % (utc, type(e).__name__, e))
                continue
            # Measure the artefact, not the status line: a short body means a
            # truncated transfer, and a truncated WAV decodes as a quiet slot.
            want = 44 + int(st["samples"]) * 2
            if len(wav) != want:
                say("  slot %d: SHORT BODY %d of %d bytes - discarded"
                    % (utc, len(wav), want))
                continue
            with open(stem + ".wav", "wb") as f:
                f.write(wav)

            period_s = 8 if proto == "FT4" else 15
            rows, freq_hz, err = decodes_for_slot(base, utc, period_s)
            note = ("this board's own decodes, NOT ground truth - see the "
                    "header of tools/js8_corpus.py")
            write_txt(stem + ".txt", utc, proto, peak,
                      float(st.get("peak_float", 0.0)), freq_hz, rows, note)
            kept += 1
            n = len(rows) if rows else 0
            say("  %s.wav  %s  %s  fpeak=%-7.1f decodes=%d%s"
                % (slot_name(utc), proto,
                     ("%.3f MHz" % (freq_hz / 1e6)) if freq_hz else "? MHz",
                     float(st.get("peak_float", 0.0)), n,
                     "  (" + err + ")" if err else ""))
            if a.slots and kept >= a.slots:
                break
    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        try:
            post_cmd(base, {"action": "slotcap", "on": False})
            print("disarmed; %d slots in %s" % (kept, a.out))
        except Exception as e:                  # noqa: BLE001
            print("WARNING: could not disarm (%s) - the board is still "
                  "recording" % e, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
