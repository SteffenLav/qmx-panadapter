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

# ⛔ A COMMAND THAT ARRIVES AT RUNTIME IS INVISIBLE TO THE GREP ABOVE.
#
# KEYING finds the literal "TX;" in OUR source. That is the right instrument
# for the six sites the firmware decides on its own, and it is blind to text
# that comes in over the network - which is how
# /api/cmd {"action":"cat_raw","cmd":"TX;"} shipped from v1.0 to v1.20.3 with
# no band guard and no authentication, while this checker reported every path
# guarded. It was telling the truth about the question it was asking.
#
# The invariant that closes it is structural rather than textual: externally
# supplied CAT text may reach the radio through exactly ONE door,
# cat_send_external_cmd(), which classifies it and consults the guard with the
# RF that would be emitted. So:
#
#   * no file that takes input from outside may call the raw senders directly,
#   * and that door must itself be guarded.
#
# A new pass-through (T63 - Ralph DL1HR's N1MM+ request) therefore cannot
# reach the radio without going past the guard, whatever strings it carries.
EXTERNAL_INPUT_DIRS = ("main/net/",)

# Reaching this from a file in EXTERNAL_INPUT_DIRS means a string from
# off-device can become a CAT command unchecked.
RAW_SENDERS = re.compile(r"\bcat_send_raw_cmd\s*\(")

# The one guarded door, and where it lives.
DOOR_NAME = "cat_send_external_cmd"
DOOR_FILE = "main/cat/cat.c"

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

    # ---- the runtime rule ------------------------------------------------
    door_src = ""
    for path in c_sources():
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError:
            continue
        if rel == DOOR_FILE:
            door_src = "".join(lines)
        if not rel.startswith(EXTERNAL_INPUT_DIRS):
            continue
        for i, line in enumerate(lines):
            stripped = line.strip()
            if stripped.startswith(("*", "//", "/*")):
                continue
            if RAW_SENDERS.search(line):
                problems.append(
                    "%s:%d  a raw CAT send in a file that takes input from "
                    "OUTSIDE the device. A command arriving over the\n"
                    "    network can carry TX; and the grep above cannot see "
                    "it - that is exactly how cat_raw shipped able to\n"
                    "    transmit out of band. Route it through %s(), which "
                    "classifies the text and consults the\n"
                    "    guard with the RF it would EMIT."
                    % (rel, i + 1, DOOR_NAME))

    # ---- and the door itself has to be guarded ---------------------------
    doors = 0
    if door_src:
        m = re.search(r"esp_err_t\s+%s\s*\([^)]*\)\s*\{" % DOOR_NAME, door_src)
        if m:
            doors = 1
            body = door_src[m.end():m.end() + 4000]
            if not GUARD.search(body):
                problems.append(
                    "%s  %s() does not call tx_band_guard_allows(). It is the "
                    "ONE door runtime CAT text passes\n"
                    "    through; unguarded, every other check here is "
                    "decoration." % (DOOR_FILE, DOOR_NAME))
            if "cat_keying_classify" not in body:
                problems.append(
                    "%s  %s() does not classify the text with "
                    "cat_keying_classify(). Without that it cannot know\n"
                    "    whether the command keys, so it guards either "
                    "everything or nothing." % (DOOR_FILE, DOOR_NAME))
        else:
            problems.append(
                "%s  %s() is missing. Runtime CAT text has no guarded door."
                % (DOOR_FILE, DOOR_NAME))

    print("check_tx_guard: %d keying site(s) found, %d exempt, "
          "%d guarded door for runtime CAT" % (found, len(EXEMPT), doors))
    # ⛔ SAY WHAT IS NOT COVERED, IN THE SAME BREATH AS THE PASS.
    #
    # Reporting "every transmit path is guarded" while a path exists that this
    # script does not examine is the same over-claim that let cat_raw ship: the
    # checker answered its own question honestly and the summary read as a
    # guarantee. So the gap is printed on every run, pass or fail.
    print("check_tx_guard: NOT COVERED - /api/term (T65). It sends "
          "browser-supplied keystrokes to the radio's own")
    print("                menu on CDC port 2, where SWR Tune keys a carrier. "
          "A keystroke's meaning depends on")
    print("                which menu row is selected, so it cannot be "
          "classified the way a CAT verb can.")
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
