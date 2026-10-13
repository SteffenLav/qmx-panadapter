#!/usr/bin/env python3
"""Every code path that can key the radio, pinned to a written inventory.

WHY THIS EXISTS
---------------
2026-10-13. Gyula HA3HZ reported that his Tab5 answered four JS8 callers with
signal reports, unattended. He was told twice that this could not happen -
once by the JS8 phase plan, which said unattended transmit arrived at phase
J10, and once by me. Both were wrong: J4 shipped in v1.17.0 and its own commit
message says "ft8_robot.c opened its pounce with a grid; it now opens with a
report in JS8". The operator then had to say publicly that he did not know what
his own release did.

The failure was not that the code was wrong. It was that nothing connected "a
new call site can key the radio" to "the operator is told". A phase plan is
prose and it went stale; a release note said "not completed" meaning "not
proven" and was read as "not implemented".

So: the set of places that arm a transmission is now a FILE, and this script is
the authority over it. Add an arming call site and the build complains until
somebody writes down what triggers it and whether a human decided. That is the
same discipline check_patches.py applies to the standing patches, and for the
same reason - a count in prose cannot keep up.

WHAT IT DOES AND DOES NOT PROVE
-------------------------------
It proves that the inventory lists exactly the arming sites that exist. It does
NOT prove the `human:` answers are right - those are read by a person and can
rot like any comment. An entry marked UNREVIEWED is reported, not fatal, so the
inventory can be adopted today and completed deliberately rather than guessed
in one sitting.

⛔ It also cannot see a path that keys the radio WITHOUT going through these
functions. If a new TX route is added, add it to ARMING_CALLS below.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INVENTORY = os.path.join(REPO, "docs", "unattended-tx-inventory.md")

# The functions that actually commit to transmitting. Their own
# implementations are skipped - only callers are sites.
ARMING_CALLS = ("ft8_tx_arm", "wspr_tx_arm")
IMPL_FILES = ("main/ft8_tx.c", "main/wspr_tx.c")

SKIP_DIRS = {".git", "build", "managed_components", "graphify-out",
             ".claude", "__pycache__", "scratchpad", "node_modules", "test"}


def _c_sources():
    for root, dirs, files in os.walk(os.path.join(REPO, "main")):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in files:
            if fn.endswith(".c"):
                yield os.path.join(root, fn)


def _rel(path):
    return os.path.relpath(path, REPO).replace("\\", "/")


_FUNC_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \*\(\)\[\],]*\b([a-z_][a-z0-9_]*)\s*\(")


def _enclosing_function(lines, idx):
    """Nearest preceding line that starts at column 0 and looks like a
    definition. Crude, and deliberately so: it only has to produce a stable
    label, not parse C."""
    for i in range(idx, -1, -1):
        line = lines[i]
        if not line or line[0] in " \t#/*}":
            continue
        m = _FUNC_RE.match(line)
        if m:
            return m.group(1)
    return "?"


def find_sites():
    sites = []
    pat = re.compile(r"\b(" + "|".join(ARMING_CALLS) + r")\s*\(")
    for path in _c_sources():
        rel = _rel(path)
        if rel in IMPL_FILES:
            continue
        try:
            text = open(path, "r", encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        lines = text.splitlines()
        for n, line in enumerate(lines):
            # a call, not a declaration or a comment
            stripped = line.lstrip()
            if stripped.startswith(("//", "*", "/*")):
                continue
            if not pat.search(line):
                continue
            sites.append((rel, _enclosing_function(lines, n), n + 1))
    return sites


# The verdict cell is allowed markdown emphasis - "**no**" is the right stress
# for a site that keys the radio on its own, so the parser tolerates it rather
# than forcing the table to be less readable.
_ENTRY_RE = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*`([^`]+)`\s*\|\s*\**\s*(yes|no|UNREVIEWED)\s*\**\s*\|",
    re.I)


def read_inventory():
    if not os.path.isfile(INVENTORY):
        return None
    entries = {}
    for line in open(INVENTORY, "r", encoding="utf-8").read().splitlines():
        m = _ENTRY_RE.match(line.strip())
        if m:
            entries[(m.group(1), m.group(2))] = m.group(3).lower()
    return entries


def run(verbose=True):
    sites = find_sites()
    have = {(f, fn) for f, fn, _ in sites}
    inv = read_inventory()

    errors, warnings = [], []

    if inv is None:
        errors.append("%s is missing - the inventory IS the authority here"
                      % _rel(INVENTORY))
        return errors, warnings

    listed = set(inv)
    missing = sorted(have - listed)       # in the code, not written down
    stale = sorted(listed - have)         # written down, no longer in the code

    for f, fn in missing:
        where = [l for ff, ffn, l in sites if (ff, ffn) == (f, fn)]
        errors.append("%s  %s()  line %s - ARMS A TRANSMISSION and is not in "
                      "the inventory. Add it, and say what triggers it."
                      % (f, fn, ", ".join(str(x) for x in where)))
    for f, fn in stale:
        errors.append("%s  %s() - in the inventory but no longer arms anything. "
                      "Remove the row." % (f, fn))

    unreviewed = sorted(k for k, v in inv.items() if v == "unreviewed" and k in have)
    for f, fn in unreviewed:
        warnings.append("%s  %s() - human decision UNREVIEWED" % (f, fn))

    if verbose:
        if errors:
            print("check_unattended_tx: INVENTORY OUT OF DATE (%d):" % len(errors))
            for e in errors:
                print("  " + e)
        if warnings:
            print("check_unattended_tx: %d site(s) not yet reviewed:" % len(warnings))
            for w in warnings:
                print("  " + w)
        unattended = sorted(k for k, v in inv.items() if v == "no" and k in have)
        print("check_unattended_tx: %d arming site(s); %d can key the radio with "
              "NO human deciding" % (len(have), len(unattended)))
        for f, fn in unattended:
            print("    unattended: %s  %s()" % (f, fn))
        if not errors and not warnings:
            print("check_unattended_tx: inventory matches the code")

    return errors, warnings


if __name__ == "__main__":
    errs, warns = run()
    print("\ncheck_unattended_tx: %d error(s), %d warning(s)" % (len(errs), len(warns)))
    sys.exit(1 if errs else 0)
