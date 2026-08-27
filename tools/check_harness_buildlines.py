#!/usr/bin/env python3
"""Do the gcc lines in the harness headers still work?

    python tools/check_harness_buildlines.py

Every harness in test/ documents its own build line in a header comment, and
that line is the ONLY instruction anyone has for running it. There is no
makefile and these are not part of `idf.py build`, so a harness whose build
line has rotted is simply a test nobody can run - and nothing says so.

That is not hypothetical. Run for the first time on 2026-08-27 it found THREE:

  - wspr_decode_harness.c, wspr_cap_sweep.c and wspr_synth_harness.c had all
    lost main/wspr_subtract.c when wspr_tones_from_message() moved there;
  - ft8_cq_encode_harness.c had lost main/ft8_msg_guard.c the same way.

The last one matters most. That harness is the regression protection for Don
WB0LQW's field report - a DOUBLE SPACE in a stored CQ preset, which
ftx_message_encode() turns into a signal report to a hashed callsign, so the
radio keys for 12.6 s and nothing decodes at the far end. The harness still
catches it perfectly; it just could not be built.

⚠ IT TESTS THE DOCUMENTATION, NOT THE CODE, and that is deliberate. It never
invents a build line of its own - if it did, a rotted header would keep working
here and keep failing for the human who copies it.

Files with NO documented build line are reported as such rather than guessed
at. Inventing one that has not been verified would be worse than none.

--run also EXECUTES each harness that built and prints its last line.

⚠ THAT IS A REPORT, NOT A VERDICT, and the distinction is deliberate. These
harnesses do not share an output format - some end "ALL PASS (0 failures)",
some "10 passed, 0 failed", some "PASSED (0 failures)", and spur_floor_harness
ends with a sentence of analysis because it is a diagnostic and has no verdict
at all. Pattern-matching that zoo would produce a checker that reads PASS when
a harness changes its wording, which is worse than no checker: a green light
nobody can trust is how a broken test survives a release. So a line that looks
like a failure is FLAGGED, and everything else is printed for a human to read.
Absence of a flag is not proof of passing.
"""
import os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__)) + "/.."
os.chdir(ROOT)

def build_line(path):
    """Join the gcc invocation out of the header comment, following '\\'."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")[:60]
    out, collecting = [], False
    for ln in lines:
        body = re.sub(r"^\s*(?:\*|//)?\s*", "", ln).rstrip()
        if not collecting and re.match(r"^(gcc|cc|clang)\b", body):
            collecting = True
        if collecting:
            cont = body.endswith("\\")
            out.append(body[:-1] if cont else body)
            if not cont:
                break
    return " ".join(out).strip() if out else None

RUN = "--run" in sys.argv

rows = []
for name in sorted(os.listdir("test")):
    if not name.endswith(".c"):
        continue
    p = os.path.join("test", name)
    cmd = build_line(p)
    # Some headers say "from test/", others "from the repo root". Running the
    # line from the wrong place is not a rotted build line, it is this script
    # being careless - and would report a false failure.
    head = open(p, encoding="utf-8", errors="replace").read()[:2000]
    cwd = os.path.join(ROOT, "test") if re.search(r"from test/", head) else ROOT
    if not cmd:
        rows.append((name, "NO BUILD LINE", ""))
        continue
    # Run only the compile half; don't execute whatever it chains with &&.
    compile_only = cmd.split("&&")[0].strip()
    tmp = tempfile.mkdtemp()
    # redirect the output binary into a temp dir so the repo stays clean
    # Only the BASENAME goes into the temp dir - a documented "-o test/x.exe"
    # would otherwise need a test/ subdirectory there, and its absence would be
    # reported as a rotted build line when the fault is entirely mine.
    compile_only = re.sub(r"-o\s+(\S+)",
                          lambda m: "-o " + tmp.replace("\\", "/") + "/h.exe",
                          compile_only)
    binpath = tmp.replace("\\", "/") + "/h.exe"
    r = subprocess.run(compile_only, shell=True, capture_output=True, text=True, cwd=cwd)
    if r.returncode == 0:
        note = ""
        if RUN:
            x = subprocess.run([binpath], capture_output=True, text=True, cwd=cwd)
            out = (x.stdout or "") + (x.stderr or "")
            tail = [l.strip() for l in out.splitlines() if l.strip()]
            note = tail[-1][:70] if tail else "(no output)"
            if re.search(r"[1-9][0-9]* (failure|failed|fail)|^ *FAIL", out, re.M):
                note = "!! " + note
        rows.append((name, "builds", note))
    else:
        err = [l for l in (r.stderr or "").split("\n")
               if "error" in l.lower() or "undefined reference" in l.lower()]
        rows.append((name, "BUILD FAILS", (err[0] if err else "").strip()[:96]))

w = max(len(r[0]) for r in rows)
bad = 0
for name, status, note in rows:
    if status != "builds":
        bad += 1
    print("  %-*s  %-14s %s" % (w, name, status, note))
print("-" * (w + 20))
print("%d of %d build from their own documented line" % (len(rows) - bad, len(rows)))
sys.exit(0)
