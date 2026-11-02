#!/usr/bin/env python3
"""Refuse the build if a T138 breadcrumb is not balanced by its exit.

WHY. panic_hook_crumb_enter() stamps RTC no-init RAM and
panic_hook_crumb_exit() clears it. A crumb still present at the next boot
means "the reset happened inside that stretch", and the next boot prints it as
such.

So an enter without its exit does not fail quietly - it makes the instrument
LIE, in the most expensive direction: every boot would report a cache-off reset
that never happened, and the depth counter would also stop every later crumb
from being recorded. A diagnostic that reports a fault on a healthy device is
worse than no diagnostic, and this project has already lost a session to an
instrument that returned confident wrong numbers (the un-normalised
correlation, 2026-10-24).

WHAT THIS CHECKS, and it is deliberately narrow rather than a C parser:
  1. Every file that enters also exits, the same number of times.
  2. No `return`, `goto`, `break`, `continue` or `vTaskDelete` sits between an
     enter and its matching exit - an early exit there skips the clear and is
     exactly fault 1 with a harder-to-see cause.

It cannot prove balance on every path through a conditional, so it is a floor,
not a proof. Keep the marked regions short and straight-line and that floor is
enough; that is also why the regions are a few statements each.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "main"

ENTER = "panic_hook_crumb_enter"
EXIT = "panic_hook_crumb_exit"
# Statements that leave the region without reaching the exit call.
ESCAPES = re.compile(r"\b(return|goto|break|continue|vTaskDelete|abort|esp_restart)\b")


def check_file(path: Path):
    failures = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as e:
        return [f"{path}: cannot read ({e})"]

    n_enter = n_exit = 0
    open_at = None          # line number of an enter awaiting its exit
    n_escapes = [0]         # list so the cap below can mutate it in place

    for i, line in enumerate(lines, 1):
        code = line.split("//", 1)[0]
        # Declarations and the header's own prose are not call sites.
        is_enter = ENTER in code and "(" in code and not code.lstrip().startswith("*")
        is_exit = EXIT in code and "(" in code and not code.lstrip().startswith("*")
        if path.name == "panic_hook.c" and ("void " + ENTER) in code:
            is_enter = False
        if path.name == "panic_hook.c" and ("void " + EXIT) in code:
            is_exit = False

        if is_enter:
            n_enter += 1
            if open_at is not None:
                failures.append(
                    f"{path.name}:{i}: a second {ENTER}() while the one at line "
                    f"{open_at} is still open. Nesting is allowed at runtime "
                    f"(the outermost owns the crumb) but it means the inner "
                    f"site can never be reported, so it is almost always a "
                    f"missing exit.")
            open_at = i
            continue
        if is_exit:
            n_exit += 1
            if open_at is None:
                failures.append(
                    f"{path.name}:{i}: {EXIT}() with no {ENTER}() before it.")
            open_at = None
            continue
        if open_at is not None and ESCAPES.search(code):
            # CAPPED, and that is not cosmetic. A missing exit makes every
            # later `return` in the file look like an escape, so the first
            # version of this printed 80 KB for one fault and buried the two
            # lines that named it. A checker whose output hides its own answer
            # is the same failure as a diagnostic that cannot be read.
            n_escapes[0] += 1
            if n_escapes[0] <= 5:
                failures.append(
                    f"{path.name}:{i}: '{ESCAPES.search(code).group(1)}' between the "
                    f"{ENTER}() at line {open_at} and its exit - that path leaves "
                    f"the crumb set, and every later boot would report a cache-off "
                    f"reset that never happened.")
            elif n_escapes[0] == 6:
                failures.append(
                    f"{path.name}: ... further escapes after the {ENTER}() at "
                    f"line {open_at} not listed. If there are many, the exit "
                    f"is probably missing entirely - see the summary below.")

    if open_at is not None:
        failures.append(
            f"{path.name}:{open_at}: {ENTER}() is never followed by "
            f"{EXIT}() in this file.")
    if n_enter != n_exit:
        failures.append(
            f"{path.name}: {n_enter} {ENTER}() vs {n_exit} {EXIT}() - "
            f"they must match.")
    return failures


def main() -> int:
    files = sorted(p for p in ROOT.rglob("*.c")
                   if ENTER in p.read_text(encoding="utf-8", errors="replace")
                   or EXIT in p.read_text(encoding="utf-8", errors="replace"))
    if not files:
        print("check_crumbs: no T138 breadcrumb sites found - has the naming "
              "changed?", file=sys.stderr)
        return 1

    failures = []
    sites = 0
    for f in files:
        failures += check_file(f)
        sites += f.read_text(encoding="utf-8", errors="replace").count(ENTER + "(")

    if failures:
        print("check_crumbs: REFUSING THE BUILD", file=sys.stderr)
        for x in failures:
            print(f"  - {x}", file=sys.stderr)
        return 1

    # panic_hook.c holds the definition, not a site.
    marked = [f.name for f in files if f.name != "panic_hook.c"]
    print(f"check_crumbs: {sites - 1} marked cache-off stretch(es) in "
          f"{len(marked)} file(s), all balanced ({', '.join(marked)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
