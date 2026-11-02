#!/usr/bin/env python3
"""Refuse the build if the drawer section ids are not unique and in range.

WHY THIS EXISTS, and it is not hypothetical twice over.

main/ui/ui.c keeps three fixed-size arrays indexed BY the DRAWER_SEC_* id:

    static lv_obj_t *s_drawer_sections[N_DRAWER_SECTIONS];
    static int       s_drawer_section_y[N_DRAWER_SECTIONS];
    static int       s_drawer_section_h[N_DRAWER_SECTIONS];

Two separate faults live in that shape:

1. OUT OF RANGE. DRAWER_SEC_REBOOT was added as 46 against a bound of 46 on
   2026-10-03, so every drawer build wrote one past the end of all three. It
   did not fault at the write - it corrupted what followed and LVGL died
   minutes later walking a garbage object pointer (Guru Meditation, Load
   access fault, lv_obj_has_flag_any <- lv_obj_is_layout_positioned).
   ui.c's _Static_asserts now catch this, but only for the ids somebody
   remembered to list, which is the same hand-kept parallel list the file
   already records going wrong twice.

2. DUPLICATE. DRAWER_SEC_CWSIM was added as 22, which DRAWER_SEC_QMXVOL
   already held (found 2026-11-02). A _Static_assert cannot see this at all:
   both ids are in range. On the CW page both sections were built, both wrote
   slot 22, and the second build won - so one row got positioned twice and
   the other's object was never positioned or hidden.

A comment cannot stop either one; this script can, and it needs no list to
keep in step because it reads the #defines themselves.
"""

import re
import sys
from pathlib import Path

UI_C = Path(__file__).resolve().parent.parent / "main" / "ui" / "ui.c"

DEFINE_RE = re.compile(
    r"^#define\s+(DRAWER_SEC_[A-Z0-9_]+)\s+(\d+)", re.MULTILINE)
BOUND_RE = re.compile(r"^#define\s+N_DRAWER_SECTIONS\s+(\d+)", re.MULTILINE)

# Ids that are deliberately dead: the section is not built, so the slot is
# never written and a shared id cannot corrupt anything. Each needs its reason
# here, in the same spirit as check_tx_guard.py's EXEMPT list - never add one
# just to silence this script.
RETIRED = {
    "DRAWER_SEC_USBSHUT": "retired 2026-08-08 - no section is built for this id",
}


def main() -> int:
    try:
        src = UI_C.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"check_drawer_sections: cannot read {UI_C}: {e}", file=sys.stderr)
        return 1

    bound_m = BOUND_RE.search(src)
    if not bound_m:
        print("check_drawer_sections: N_DRAWER_SECTIONS not found in ui.c",
              file=sys.stderr)
        return 1
    bound = int(bound_m.group(1))

    names = DEFINE_RE.findall(src)
    if not names:
        print("check_drawer_sections: no DRAWER_SEC_* defines found - has the "
              "naming changed?", file=sys.stderr)
        return 1

    by_id = {}
    failures = []

    for name, raw in names:
        sec_id = int(raw)
        if sec_id >= bound:
            failures.append(
                f"{name} = {sec_id} is >= N_DRAWER_SECTIONS ({bound}). Every "
                f"drawer build would write past the end of "
                f"s_drawer_sections[] / _y[] / _h[]. Raise the bound.")
        by_id.setdefault(sec_id, []).append(name)

    for sec_id, holders in sorted(by_id.items()):
        live = [h for h in holders if h not in RETIRED]
        if len(live) > 1:
            failures.append(
                f"id {sec_id} is used by {len(live)} live sections: "
                f"{', '.join(live)}. The three s_drawer_section* arrays are "
                f"indexed by this id, so whichever section is built last wins "
                f"the slot and the others are never positioned or hidden. "
                f"Give one of them a free id below {bound}.")

    if failures:
        print("check_drawer_sections: REFUSING THE BUILD", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    used = sorted(by_id)
    free = [i for i in range(bound) if i not in by_id]
    print(f"check_drawer_sections: {len(names)} sections, ids unique and "
          f"< {bound} (highest {used[-1]}; free: "
          f"{', '.join(str(i) for i in free) if free else 'none'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
