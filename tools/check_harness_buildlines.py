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

# Reading and running a header line: tools/harness_lines.py, shared with
# run_harnesses.py so the two can never disagree about a harness again.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness_lines as HL  # noqa: E402

ROOT = os.path.dirname(os.path.abspath(__file__)) + "/.."
os.chdir(ROOT)

RUN = "--run" in sys.argv

rows = []
for name in sorted(os.listdir("test")):
    if not name.endswith(".c"):
        continue
    p = os.path.join("test", name)
    # A harness that owns a build script says so with an `sh ....sh` line, and
    # tools/run_harnesses.py already prefers it. This checker did not, so it
    # built js8_submode_table_harness.c from a gcc line the header itself says
    # is NOT the way to build it - a false failure, 2026-11-21. The script
    # builds AND runs, so it is run whole.
    sh_line = HL.script_line(p)
    if sh_line:
        r = HL.run(sh_line, cwd=ROOT)
        rows.append((name, "builds (script)" if r.returncode == 0 else "SCRIPT FAILS",
                     "" if r.returncode == 0 else (r.stderr or r.stdout).strip()[-120:]))
        continue
    cmd = HL.gcc_line(p)
    # Some headers say "from test/", others "from the repo root". Running the
    # line from the wrong place is not a rotted build line, it is this script
    # being careless - and would report a false failure.
    head = open(p, encoding="utf-8", errors="replace").read()[:2000]
    # ⛔ "from test/" ALONE MATCHED "Run over the whole corpus from
    # test/wav_reference" (ft8_decode_bench.c) and built it in the wrong place
    # - a false failure, 2026-11-21. Now: a "Build (from test/)" header, or a
    # build line that itself reaches up with ../ - either one means test/.
    from_test = (re.search(r"Build\s*\(from test/\)", head)
                 or (cmd and re.search(r"(^|\s)(-I)?\.\./", cmd)))
    cwd = os.path.join(ROOT, "test") if from_test else ROOT
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
    # ⛔ THROUGH BASH, NOT cmd.exe. The lines are written for a POSIX shell
    # and several use brace lists ({decode,encode,...}.c), which cmd.exe
    # passes through literally - so they "failed" here while working for
    # anyone who copied them into Git Bash. A false failure, 2026-11-21.
    r = HL.run(compile_only, cwd=cwd)
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
