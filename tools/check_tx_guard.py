#!/usr/bin/env python3
"""Refuse the build if anything can put RF out without passing the band guard.

WHY THIS EXISTS, and why it is a script rather than a comment.

2026-10-21: the WSPR 60 m dial emitted 63 kHz below the bottom of the band, in
every IARU region, and shipped - because NOTHING checked. The band plan drove
the display strip, the DX cluster and the web status, and never refused a
transmission. Transmitting out of band is the operator's licence on the line,
so the device has to prohibit it (his words, 2026-10-21: "TX'ing out of band no
matter what mode is a legal thing that potentially can fall back on my
shoulders if not prohibited by my device").

Having fixed the six sites that exist today, the question is the SEVENTH - the
one somebody adds next year. CLAUDE.md records the same rot twice already (the
standing-patch count and the unattended-TX count both drifted in prose while a
checker was the authority), so this follows that pattern:

  * find every site that can cause RF,
  * require each to be either guarded or EXPLICITLY exempted with a reason,
  * fail the build otherwise.

A new keying site therefore cannot ship unnoticed. It can still ship exempted,
but only by someone writing down why.

Run it standalone, or let the build run it:

    python tools/check_tx_guard.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(ROOT, "main")

# What makes RF on a QMX, as CAT commands we send:
#   TX;        key down
#   MD8 / TUNE SWR Tune mode - the radio keys a carrier by itself
# TA<freq>; is the audio tone that modulates an already-keyed DiGi carrier; it
# makes no RF on its own, so it is not a keying site. KY/CW are not here
# because this firmware never keys CW - it sets the keyer speed and the split
# offset, and the operator keys with a paddle on the radio.
KEYING = [
    (re.compile(r'"TX;"'),                 "TX; key-down"),
    (re.compile(r'cat_request_mode\("TUNE"\)'), "SWR Tune (MD8) carrier"),
]

# How far above a keying site the guard may sit and still count. The guard is a
# pre-flight, so it is always a short way above in the same function.
WINDOW = 60

GUARD = re.compile(r"tx_band_guard_allows\s*\(")

# Sites that are allowed to key WITHOUT the guard. Each needs a reason, and the
# reason is the point: anyone adding a line here is making a decision, not
# silencing a warning.
EXEMPT = {
    ("main/ft8_tx.c", "TX; key-down", "SWR latch clear"):
        "Reachable only from inside a burst that passed the guard seconds "
        "earlier, on the same dial and tone. Refusing would leave the radio "
        "with a tripped SWR latch - worse than the 150 ms it keys for.",
}


def c_sources():
    for dirpath, _dirs, files in os.walk(MAIN):
        for f in files:
            if f.endswith(".c"):
                yield os.path.join(dirpath, f)


def main():
    problems = []
    found = 0
    for path in c_sources():
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError as e:
            problems.append("%s: cannot read (%s)" % (rel, e))
            continue
        for i, line in enumerate(lines):
            # A commented-out or quoted mention is not a call site.
            stripped = line.strip()
            if stripped.startswith(("*", "//", "/*")):
                continue
            for pat, what in KEYING:
                if not pat.search(line):
                    continue
                found += 1
                lo = max(0, i - WINDOW)
                context = "".join(lines[lo:i + 1])
                if GUARD.search(context):
                    continue
                # exempt?
                why = None
                for (ef, ew, _tag), reason in EXEMPT.items():
                    if ef == rel and ew == what:
                        why = reason
                        break
                if why:
                    continue
                problems.append(
                    "%s:%d  %s is NOT band-guarded.\n"
                    "    Call tx_band_guard_allows() with the RF this will "
                    "EMIT (dial + audio offset, or the dial itself for a plain\n"
                    "    carrier) before it keys, and do not transmit when it "
                    "returns false. If it genuinely must not be guarded, add\n"
                    "    it to EXEMPT in this file WITH THE REASON."
                    % (rel, i + 1, what))

    print("check_tx_guard: %d keying site(s) found, %d exempt"
          % (found, len(EXEMPT)))
    if problems:
        print("\n" + "=" * 78)
        print("check_tx_guard: UNGUARDED TRANSMIT PATH - REFUSING THE BUILD")
        print("=" * 78 + "\n")
        for p in problems:
            print("  " + p)
        print("\n  Transmitting out of band is the operator's licence, not a "
              "cosmetic defect.")
        return 1
    print("check_tx_guard: every transmit path is guarded or explicitly exempt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
