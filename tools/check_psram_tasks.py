#!/usr/bin/env python3
"""Refuse the build if a PSRAM-stack task can leak its stack.

psram_task_create*() gives a task a stack and TCB that WE own; FreeRTOS does
not free them on vTaskDelete() (main/util/psram_task.h, #279). So:

  * a task created with psram_task_create_reapable() must end with
    psram_task_park() and must never call vTaskDelete();
  * a task created with plain psram_task_create() is for the life of the boot
    and must never call vTaskDelete() either.

2026-11-11: wspr_tx_sched_task (16 KB, every WSPR visit) and reader_net's
fetch_task (4 KB, every manual page) both ended with vTaskDelete(NULL). The
rule had been written in the header since #279; a comment did not stop it.

Exit 0 = clean, 1 = a violation (printed with file and function).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "main"

# Permanent tasks whose ONLY vTaskDelete() is a start-up failure exit, before
# the task's forever loop: at most one leak per boot, and only when start-up
# already failed. Making them reapable would hold one of the 16 reap slots for
# the life of the boot - permanent tasks filling the table is what broke the
# first version of psram_task (see psram_task.h). Every entry says why.
EXEMPT = {
    "band_conditions_task": "permanent; delete only if start-up fails, once per boot at most",
    "diag_persist_task": "permanent; deletes only on a start-up / fatal storage path, once per boot at most",
    "flush_task": "permanent settings flusher; delete only if start-up fails",
    "qrz_task": "permanent; delete only if start-up fails, once per boot at most",
}
CREATE = re.compile(r"\bpsram_task_create(_reapable)?\s*\(\s*([A-Za-z_]\w*)\s*,")


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def func_body(src: str, name: str):
    m = re.search(r"^(?:static\s+)?void\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", src, re.M)
    if not m:
        return None
    i, depth = m.end(), 1
    while i < len(src) and depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    return src[m.end():i]


def main() -> int:
    files = {p: strip_comments(p.read_text(encoding="utf-8", errors="replace"))
             for p in ROOT.rglob("*.c") if p.name != "psram_task.c"}
    created = {}  # func -> reapable?
    for src in files.values():
        for m in CREATE.finditer(src):
            created[m.group(2)] = created.get(m.group(2), False) or bool(m.group(1))
    bad, checked = [], 0
    for func, reapable in sorted(created.items()):
        if func in EXEMPT and not reapable:
            continue
        hits = [(p, b) for p, s in files.items() if (b := func_body(s, func)) is not None]
        if not hits:
            bad.append(f"{func}: created with psram_task_create*() but its body was not found")
            continue
        for p, body in hits:
            checked += 1
            rel = p.relative_to(ROOT.parent).as_posix()
            if re.search(r"\bvTaskDelete\s*\(", body):
                bad.append(f"{rel}: {func}() calls vTaskDelete() - leaks its PSRAM stack; "
                           f"use psram_task_park() and create it with psram_task_create_reapable()")
            if reapable and "psram_task_park" not in body:
                bad.append(f"{rel}: {func}() is created reapable but never calls psram_task_park()")
    for b in bad:
        print("check_psram_tasks: " + b)
    print(f"check_psram_tasks: {checked} PSRAM task(s) checked, {len(bad)} violation(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
