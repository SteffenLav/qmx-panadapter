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
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GCCDIR = Path(os.environ["LOCALAPPDATA"]) / (
    "Microsoft/WinGet/Packages/"
    "BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
) if os.name == "nt" else None

# Reading and running a header's build line lives in tools/harness_lines.py,
# shared with check_harness_buildlines.py. This file had its own copy that
# read only 40 lines and ran through cmd.exe - see that module for what it
# cost (2026-11-21).
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness_lines as HL  # noqa: E402

SH = HL.BASH


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
        if HL.is_measurement(src):
            print(f"  {name:<26} SKIP  (measurement tool, not pass/fail)")
            nskip += 1
            continue
        script = HL.script_line(src)
        if script:
            if not SH:
                print(f"  {name:<26} SKIP  (no POSIX sh on this machine)")
                nskip += 1
                continue
            # Hand it an absolute sh rather than hoping the parent shell has
            # one. `label` keeps the ORIGINAL line, because the quoted sh path
            # contains spaces and script.split()[1] would print a fragment of
            # "Program Files" as the script name.
            label = script.split()[1]
            r = HL.run(script, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                       timeout=600)
            if r.returncode == 0:
                print(f"  {name:<26} pass  (via {label})")
                npass += 1
            else:
                print(f"  {name:<26} RUN FAIL  (via {label})")
                failures.append((name, "script", (r.stdout + r.stderr).strip()))
                nfail += 1
            continue

        cmd = HL.gcc_line(src)
        if not cmd:
            print(f"  {name:<26} SKIP  (no build line in its header)")
            nskip += 1
            continue

        # Redirect the output binary into the temp dir; harness headers name it
        # relative to wherever the author happened to be standing.
        exe = out / (name + ".exe")
        # A Windows path in the REPLACEMENT is not a template - "C:\Users" would
        # be read as an escape. Pass a function so it is taken literally.
        cmd = re.sub(r"-o\s+\S+", lambda _m: f'-o "{exe.as_posix()}"', cmd, count=1)

        # Some headers document their command as run FROM test/ (relative paths
        # like "psk_harness.c" or "../main/adif/lotw_tq8.c"), others from the
        # repo root. Try the root, then test/ - which of the two an author was
        # standing in is not worth encoding in 27 places.
        r = HL.run(cmd, cwd=ROOT, env=env)
        if (r.returncode != 0 or not exe.exists()):
            r2 = HL.run(cmd, cwd=ROOT / "test", env=env)
            if r2.returncode == 0 and exe.exists():
                r = r2
        if r.returncode != 0 or not exe.exists():
            print(f"  {name:<26} BUILD FAIL")
            failures.append((name, "build", (r.stderr or r.stdout).strip()))
            nfail += 1
            continue

        # stdin=DEVNULL, deliberately. country_shorten_harness reads lines
        # from stdin and stops at EOF; with stdin inherited it blocks until
        # the 300 s timeout whenever the parent's stdin stays open - which is
        # every background run. It passed from an interactive shell and hung
        # from a backgrounded one, twice on 2026-10-08, and the difference was
        # never the harness. Any future harness that reads stdin gets EOF
        # immediately rather than hanging the whole suite.
        r = subprocess.run([str(exe)], cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                           capture_output=True, text=True, timeout=300)
        if r.returncode == 0:
            print(f"  {name:<26} pass")
            npass += 1
        else:
            print(f"  {name:<26} RUN FAIL")
            failures.append((name, "run", (r.stdout + r.stderr).strip()))
            nfail += 1

    # Node harnesses (*_harness.mjs): they test the web page's own JavaScript,
    # so there is nothing to compile - run them with node. Skipped, not failed,
    # on a machine without node: the firmware build does not need it.
    node = shutil.which("node")
    for src in sorted((ROOT / "test").glob("*_harness.mjs")):
        name = src.stem
        if only and only not in name:
            continue
        if not node:
            print(f"  {name:<26} SKIP  (no node on this machine)")
            nskip += 1
            continue
        r = subprocess.run([node, str(src)], cwd=ROOT, env=env,
                           stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, encoding="utf-8", errors="replace",
                           timeout=300)
        if r.returncode == 0:
            print(f"  {name:<26} pass  (node)")
            npass += 1
        else:
            print(f"  {name:<26} RUN FAIL  (node)")
            failures.append((name, "run", (r.stdout + r.stderr).strip()))
            nfail += 1

    print(f"\nharnesses: {npass} passed, {nfail} failed, {nskip} skipped")
    for name, phase, msg in failures:
        print(f"\n--- {name} ({phase}) ---")
        print("\n".join(msg.splitlines()[-6:]))
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
