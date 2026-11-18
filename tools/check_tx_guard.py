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
# makes no RF on its own, so it is not a keying site.
#   KY <text>; the radio's own keyer sends the text - RF for as long as it takes
#
# ⛔ KY WAS MISSING FROM THIS LIST UNTIL 2026-11-10. The comment here said "this
# firmware never keys CW - the operator keys with a paddle". True when written,
# false from the day the CW page shipped, and this script went on reporting
# every path guarded while the CW page's KY send had no band check. Found while
# adding type-ahead. Both wire forms are matched: the plain one and the TS480
# fixed-length experiment.
KEYING = [
    (re.compile(r'"TX;"'),                 "TX; key-down"),
    (re.compile(r'cat_request_mode\("TUNE"\)'), "SWR Tune (MD8) carrier"),
    (re.compile(r'"KY ?%s;"'),             "KY CW send"),
]

# How far above a keying site the guard may sit and still count. The guard is a
# pre-flight, so it is always a short way above in the same function.
WINDOW = 60

GUARD = re.compile(r"tx_band_guard_allows\s*\(")

# ⛔ THE SECOND LOCK: A WEB REQUEST FROM OFF-SUBNET MAY NOT KEY THE RADIO (T206).
#
# The band guard above answers "may this frequency be transmitted on". It says
# nothing about WHO asked, and the web API has no authentication at all - no
# Authorization handling, no key, no token, no session, and no TLS to carry one
# over. So a Tab5 that is port-forwarded, deliberately or by a router doing it
# unasked, is a transmitter anyone can key. main/tx_subnet.h holds the whole
# argument, including why the login itself belongs in a tunnel and not here.
#
# The same shape as the rule above: find every web path that can cause RF,
# require each to consult the interlock or be exempted WITH A REASON.
#
# ⚠ WHAT THIS CANNOT SEE, stated here rather than left to be discovered: the
# list below is by NAME. A new web TX route that arms through some function not
# named here is invisible to this rule, exactly as docs/unattended-tx-inventory
# records for its own ARMING_CALLS. Add new arming entry points to BOTH.
SUBNET_GUARD = re.compile(r"tx_subnet_allows_(req|fd)\s*\(|tx_subnet_cmd_ok\s*\(")

# Functions that commit to transmitting, or hand a request to the task that
# will. Reached from a file that takes input from off-device, each one needs
# the interlock first.
WEB_TX_CALLS = [
    (re.compile(r"\bft8_tx_arm\s*\("),                            "ft8_tx_arm"),
    (re.compile(r"\bwspr_tx_arm\s*\("),                           "wspr_tx_arm"),
    (re.compile(r"\brtty_tx_send\s*\("),                          "rtty_tx_send"),
    (re.compile(r"\brtty_tx_macro\s*\("),                         "rtty_tx_macro"),
    (re.compile(r"\bcat_request_cw_send\s*\("),                   "cat_request_cw_send"),
    (re.compile(r"\bcw_screen_view_request_send\s*\("),           "CW send request"),
    (re.compile(r"\bft8_screen_view_request_freetext\s*\("),      "JS8 free-text request"),
    (re.compile(r"\bft8_screen_view_request_cq\s*\("),            "CQ-run request"),
    (re.compile(r"\bft8_screen_view_request_reply(_retarget)?\s*\("), "FT8/JS8 reply request"),
    (re.compile(r"\bft8_screen_view_request_js8_macro\s*\("),     "JS8 macro request"),
    (re.compile(r"\bft8_screen_view_request_js8_directive\s*\("), "JS8 directive request"),
    (re.compile(r"\bft8_screen_view_request_override\s*\("),      "FT8 override request"),
    (re.compile(r"\bjs8_ftx_start\s*\("),                         "js8_ftx_start"),
    (re.compile(r'\bcat_request_mode\("TUNE"\)'),                  "SWR Tune (MD8) carrier"),
    (re.compile(r"\bqmx_term_key_checked\s*\("),                  "QMX terminal keystroke"),
]

# Files that take input from off the device. Same set as EXTERNAL_INPUT_DIRS -
# kept as its own name because the two rules could one day differ.
WEB_TX_DIRS = ("main/net/",)

# How far above a site the interlock may sit. LARGER than the band guard's 60:
# the interlock is a per-ACTION pre-flight at the top of an /api/cmd branch,
# and a branch that validates its arguments first (wspr_tx_test builds and
# checks a whole request) legitimately runs long before it arms.
SUBNET_WINDOW = 120

# Web TX sites allowed to skip the interlock. A reason, not a silence.
SUBNET_EXEMPT = {
    ("main/net/webserver.c", "FT8 override request"):
        "The branch guards only what TRANSMITS (resend/rr73/73). cancel, pause "
        "and skip are how a remote operator STOPS the station, and the "
        "interlock must never be the reason a transmitter stays keyed - "
        "feedback_never_leave_the_radio_transmitting outranks who is asking. "
        "The guarded call and the exempt one are the same line, so the rule "
        "below cannot tell them apart; the `what >= 1 && what <= 3` condition "
        "at the site is the real split.",
}

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

# The guarded doors runtime input has to pass, and where each lives.
#
#   CAT port (T64)      - text from the web API or a future PC pass-through.
#   terminal port (T65) - keystrokes forwarded to the radio's OWN menus, where
#                         the sweeps, SSB Calibration, Test ADC I/Q, PA mod.
#                         test and the beacon all key the transmitter, and `t`
#                         on the Diagnostics screen does it at full PA voltage
#                         with no SWR protection. Entering Diagnostics does
#                         NOT key - see main/qmx_term_tx_rows.c.
#
# Each door must consult its classifier. The CAT door also calls the band
# guard; the terminal door REFUSES instead, because a sweep emits across a
# whole filter passband - there is no single frequency to hand the guard, and
# most of what it emits is outside every allocation by design. See
# main/qmx_term_tx_rows.h.
DOORS = [
    {
        "name": "cat_send_external_cmd",
        "file": "main/cat/cat.c",
        "classifier": ["cat_keying_classify"],
        "classifier_in_file": "cat_keying_classify",
        "needs_band_guard": True,
        # T206: the door that classifies the text also decides whether the
        # CALLER may transmit at all. Both, in that order - the cheapest
        # refusal is the one that never looked at the dial.
        "needs_subnet_interlock": True,
        "sinks": re.compile(r"\bcat_send_raw_cmd\s*\("),
        "sink_dirs": ("main/net/",),
        "sink_what": "a raw CAT send",
    },
    {
        "name": "qmx_term_key_checked",
        "file": "main/qmx_term.c",
        # The door asks two helpers next door in the same file; they are what
        # hold the screen lock. The real classifier has to be in the file.
        "classifier": ["enter_would_transmit",
                       "qmx_term_key_transmits_on_diagnostics"],
        "classifier_in_file": "qmx_term_row_transmits",
        "needs_band_guard": False,
        # The terminal door refuses every row it knows transmits, so it has no
        # keying to interlock. The interlock sits on /api/term in webserver.c
        # instead, where the request and its socket are - and it is needed
        # there because this door matches rows by LABEL from the manual, so a
        # reworded row is unguarded and several of those rows key at full PA
        # voltage with no SWR protection.
        "needs_subnet_interlock": False,
        # The unchecked wrapper is GONE, so the rule is absolute: this name
        # must not appear in any .c but the door's own file.
        "sinks": re.compile(r"\bqmx_term_key\s*\("),
        "sink_dirs": ("main/",),
        "sink_what": "a terminal keystroke that skips the transmit check",
    },
]

# Kept for the message text below.
DOOR_NAME = DOORS[0]["name"]
DOOR_FILE = DOORS[0]["file"]

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
    door_src = {}
    for path in c_sources():
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError:
            continue
        for d in DOORS:
            if rel == d["file"]:
                door_src[rel] = "".join(lines)
        for d in DOORS:
            if not d["sink_dirs"] or not rel.startswith(d["sink_dirs"]):
                continue
            if rel == d["file"]:
                continue
            for i, line in enumerate(lines):
                stripped = line.strip()
                if stripped.startswith(("*", "//", "/*")):
                    continue
                if d["sinks"].search(line):
                    problems.append(
                        "%s:%d  %s in a file that takes input from OUTSIDE the "
                        "device. Input arriving over the\n"
                        "    network can carry a transmit command and the grep "
                        "above cannot see it - that is exactly how\n"
                        "    cat_raw shipped able to transmit out of band. "
                        "Route it through %s()."
                        % (rel, i + 1, d["sink_what"], d["name"]))

    # ---- the TX interlock: who asked, not just what they asked (T206) ----
    web_tx_sites = 0
    for path in c_sources():
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        if not rel.startswith(WEB_TX_DIRS):
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError:
            continue
        for i, line in enumerate(lines):
            stripped = line.strip()
            if stripped.startswith(("*", "//", "/*")):
                continue
            for pat, what in WEB_TX_CALLS:
                if not pat.search(line):
                    continue
                web_tx_sites += 1
                lo = max(0, i - SUBNET_WINDOW)
                if SUBNET_GUARD.search("".join(lines[lo:i + 1])):
                    continue
                if (rel, what) in SUBNET_EXEMPT:
                    continue
                problems.append(
                    "%s:%d  %s can transmit on behalf of a request from "
                    "ANYWHERE - the TX interlock (T206) is not consulted.\n"
                    "    The web API has no authentication and no TLS, so a "
                    "port-forwarded Tab5 is a transmitter anyone can key.\n"
                    "    Call tx_subnet_allows_req() (or tx_subnet_cmd_ok() in "
                    "webserver.c) before it arms, and answer the CALLER\n"
                    "    with the refusal - not only the log. If it genuinely "
                    "must be exempt, add it to SUBNET_EXEMPT in this\n"
                    "    file WITH THE REASON."
                    % (rel, i + 1, what))

    # ---- and each door itself has to check -------------------------------
    doors = 0
    for d in DOORS:
        src = door_src.get(d["file"], "")
        if not src:
            problems.append("%s  cannot read; %s() is unverified"
                            % (d["file"], d["name"]))
            continue
        m = re.search(r"\b(?:esp_err_t|bool)\s+%s\s*\([^)]*\)\s*\{" % d["name"], src)
        if not m:
            problems.append(
                "%s  %s() is missing. Runtime input has no guarded door there."
                % (d["file"], d["name"]))
            continue
        doors += 1
        body = src[m.end():m.end() + 4000]
        missing = [c for c in d["classifier"] if c not in body]
        if missing:
            problems.append(
                "%s  %s() does not consult %s.\n"
                "    Without that it cannot know whether the input keys, so it "
                "refuses either everything or nothing."
                % (d["file"], d["name"], ", ".join(m + "()" for m in missing)))
        if d["classifier_in_file"] not in src:
            problems.append(
                "%s  does not use %s(), so the door has no classifier to ask."
                % (d["file"], d["classifier_in_file"]))
        if d.get("needs_subnet_interlock") and not SUBNET_GUARD.search(body):
            problems.append(
                "%s  %s() does not consult the TX interlock "
                "(tx_subnet_allows_fd, T206).\n"
                "    It is the one door runtime CAT text passes, and nothing "
                "else knows which socket the text came in on."
                % (d["file"], d["name"]))
        if d["needs_band_guard"] and not GUARD.search(body):
            problems.append(
                "%s  %s() does not call tx_band_guard_allows(). It is a door "
                "runtime input passes through;\n"
                "    unguarded, every other check here is decoration."
                % (d["file"], d["name"]))

    print("check_tx_guard: %d keying site(s) found, %d exempt, "
          "%d guarded door(s) for runtime input" % (found, len(EXEMPT), doors))
    print("check_tx_guard: %d web TX site(s) behind the subnet interlock, "
          "%d exempt (T206)" % (web_tx_sites, len(SUBNET_EXEMPT)))
    # ⛔ SAY WHAT IS NOT COVERED, IN THE SAME BREATH AS THE PASS.
    #
    # Reporting "every transmit path is guarded" while a path exists that this
    # script does not examine is the same over-claim that let cat_raw ship: the
    # checker answered its own question honestly and the summary read as a
    # guarantee. So the limits are printed on every run, pass or fail.
    print("check_tx_guard: LIMITS - the terminal door (T65) matches menu rows "
          "by LABEL, from the 1_04_001")
    print("                manual, not from a radio. A reworded row is "
          "unguarded, and a row it cannot read")
    print("                while the screen repaints is ALLOWED. The radio's "
          "own front panel is never gated.")
    print("check_tx_guard: LIMITS - the TX interlock (T206) is matched by the "
          "NAMES in WEB_TX_CALLS. A new web")
    print("                TX route arming through some other function is "
          "invisible to it. The interlock is")
    print("                also DEFAULT OFF and is a subnet test, not a login: "
          "it stops the internet, not a")
    print("                guest on the WiFi, and it checks nothing at all "
          "while the setting is off.")
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
