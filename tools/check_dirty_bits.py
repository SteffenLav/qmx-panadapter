#!/usr/bin/env python3
"""Fail the build when two settings share a dirty-bit index.

WHY THIS EXISTS

storage/settings.c hands every persisted setting a bit index in a 128-bit
array, as a plain `#define DIRTY_x n` written by hand. Two features developed
on different branches will pick the same free number, and MERGING THEM IS
SILENT: both defines survive, the compiler is happy, and nothing at runtime
complains.

That is not hypothetical. Merging feat/wspr-page with main produced exactly
this - DIRTY_WSPR_DIAL and DIRTY_DRAWER_EXPERT both at 97, because 96 really
was the highest index in use when the WSPR branch was cut and main had since
taken 97. The WSPR block's own comment still said "96 was the highest index in
use" while that was no longer true.

WHAT A COLLISION ACTUALLY COSTS

Mostly it is a silent extra NVS write: marking either bit makes the flush write
BOTH keys, each from the same snapshot, so the values stay correct. It turns
into a real fault the moment either bit joins s_config_export_bits, because
then an unrelated setting triggers the microSD config mirror - and CLAUDE.md
records at length how a spurious SD mirror kills WiFi on this board. It would
also break any code that clears one bit expecting the other to survive.

So the damage is latent rather than immediate, which is precisely why it needs
a build-time check: nothing was ever going to notice it by using the device.

WHY NOT AN ENUM

An enum cannot collide and would be the real fix. It is not done here because
renumbering every bit is a large mechanical diff across ~100 call sites, and
this check buys the same protection for a few lines. If settings.c is ever
refactored anyway, prefer the enum and delete this.
"""
import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SETTINGS = os.path.join(HERE, "..", "main", "storage", "settings.c")

# DIRTY_WORDS / DIRTY_BITS_MAX size the array; they are not bit indices.
NOT_AN_INDEX = {"DIRTY_WORDS", "DIRTY_BITS_MAX"}

DEF = re.compile(r"^#define\s+(DIRTY_[A-Z0-9_]+)\s+(\d+)\s*(?:/\*|//|$)", re.M)


def main():
    try:
        with open(SETTINGS, encoding="utf-8", errors="replace") as fh:
            src = fh.read()
    except OSError as exc:
        print("check_dirty_bits: cannot read %s (%s)" % (SETTINGS, exc))
        return 0          # do not fail a build over a moved file

    seen = {}
    collisions = []
    highest = -1
    for name, idx in DEF.findall(src):
        if name in NOT_AN_INDEX:
            continue
        idx = int(idx)
        highest = max(highest, idx)
        if idx in seen:
            collisions.append((idx, seen[idx], name))
        else:
            seen[idx] = name

    # The array width, so "there is room" is checked rather than assumed.
    m = re.search(r"^#define\s+DIRTY_WORDS\s+(\d+)", src, re.M)
    words = int(m.group(1)) if m else 0
    capacity = words * 32

    if collisions:
        print("")
        print("  ERROR: settings.c has %d dirty-bit collision(s)." % len(collisions))
        for idx, a, b in collisions:
            print("    index %-4d %s  AND  %s" % (idx, a, b))
        print("")
        print("  Give one of each pair the next free index. Free now: %d..%d"
              % (highest + 1, capacity - 1))
        print("  The map is runtime-only - nothing in NVS depends on the")
        print("  numbers - so renumbering either side is safe.")
        print("")
        return 1

    if capacity and highest >= capacity:
        print("")
        print("  ERROR: dirty bit %d does not fit in DIRTY_WORDS=%d (%d bits)."
              % (highest, words, capacity))
        print("  Raise DIRTY_WORDS in settings.c - that one number is the width.")
        print("")
        return 1

    print("check_dirty_bits: %d settings bits, no collisions, %d free"
          % (len(seen), capacity - highest - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
