#!/usr/bin/env python3
"""Build and run every host harness in test/, and report pass/fail.

Each harness links the REAL function rather than a copy of its logic - that is
the whole point of the portable-file split (util/db_gridlines.c,
util/net_guard.c, adif/adif_check.c, util/format_freq.c and the rest). A
harness that mirrored the logic would test the mirror.

The build command is read from each harness's own header comment, so there is
no second copy here to drift from it.

WHY THIS EXISTS: this machine had NO host C compiler until 2026-09-06, so the
suite had never been run on it at all and two harnesses written that day had
never even been compiled. Install the toolchain with:

    winget install BrechtSanders.WinLibs.POSIX.UCRT

Usage:  python tools/run_harnesses.py [name-substring]
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GCCDIR = Path(os.environ["LOCALAPPDATA"]) / (
    "Microsoft/WinGet/Packages/"
    "BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
) if os.name == "nt" else None

LEADER = re.compile(r"^\s*(\*|//|/\*)\s?")


def build_command(src: Path):
    """The gcc line from the header, following backslash continuations."""
    parts, collecting = [], False
    for raw in src.read_text(encoding="utf-8", errors="replace").splitlines()[:40]:
        # The leader strip leaves the header's own indentation ("*   gcc ..."),
        # so lstrip before matching or nothing is ever found.
        line = LEADER.sub("", raw).strip()
        if not collecting and line.startswith("gcc "):
            collecting = True
        if collecting:
            cont = line.endswith("\\")
            parts.append(line[:-1].rstrip() if cont else line)
            if not cont:
                break
    if not parts:
        return None
    cmd = " ".join(parts)
    for cut in ("&&", ";"):                    # drop the "&& ./harness" tail
        cmd = cmd.split(cut)[0]
    return " ".join(cmd.split())


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    env = dict(os.environ)
    if GCCDIR and GCCDIR.is_dir():
        env["PATH"] = str(GCCDIR) + os.pathsep + env["PATH"]

    out = Path(tempfile.mkdtemp())
    npass = nfail = nskip = 0
    failures = []

    for src in sorted((ROOT / "test").glob("*_harness.c")):
        name = src.stem
        if only and only not in name:
            continue
        cmd = build_command(src)
        if not cmd:
            print(f"  {name:<26} SKIP  (no build line in its header)")
            nskip += 1
            continue

        # Redirect the output binary into the temp dir; harness headers name it
        # relative to wherever the author happened to be standing.
        exe = out / (name + ".exe")
        # A Windows path in the REPLACEMENT is not a template - "C:\Users" would
        # be read as an escape. Pass a function so it is taken literally.
        cmd = re.sub(r"-o\s+\S+", lambda _m: f'-o "{exe}"', cmd, count=1)

        # Some headers document their command as run FROM test/ (relative paths
        # like "psk_harness.c" or "../main/adif/lotw_tq8.c"), others from the
        # repo root. Try the root, then test/ - which of the two an author was
        # standing in is not worth encoding in 27 places.
        r = subprocess.run(cmd, shell=True, cwd=ROOT, env=env,
                           capture_output=True, text=True)
        if (r.returncode != 0 or not exe.exists()):
            r2 = subprocess.run(cmd, shell=True, cwd=ROOT / "test", env=env,
                                capture_output=True, text=True)
            if r2.returncode == 0 and exe.exists():
                r = r2
        if r.returncode != 0 or not exe.exists():
            print(f"  {name:<26} BUILD FAIL")
            failures.append((name, "build", (r.stderr or r.stdout).strip()))
            nfail += 1
            continue

        r = subprocess.run([str(exe)], cwd=ROOT, env=env,
                           capture_output=True, text=True, timeout=300)
        if r.returncode == 0:
            print(f"  {name:<26} pass")
            npass += 1
        else:
            print(f"  {name:<26} RUN FAIL")
            failures.append((name, "run", (r.stdout + r.stderr).strip()))
            nfail += 1

    print(f"\nharnesses: {npass} passed, {nfail} failed, {nskip} skipped")
    for name, phase, msg in failures:
        print(f"\n--- {name} ({phase}) ---")
        print("\n".join(msg.splitlines()[-6:]))
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
