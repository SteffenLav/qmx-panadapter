"""How a host harness says it is built - ONE parser for every tool that asks.

tools/run_harnesses.py and tools/check_harness_buildlines.py each had their
own copy, and they disagreed (2026-11-21):
  - the runner read 40 header lines and the checker 60, so a build line that
    started at line 36 was a half command in one and fine in the other;
  - both ran it through cmd.exe, which passes {a,b} brace lists through
    literally, so every brace-using line failed for the tools while it worked
    for a person in Git Bash;
  - only the runner knew that an `sh ...sh` line overrides the gcc one.
Anything about reading or running a header line belongs here.
"""
import os, re, shutil, subprocess

# The leading comment of a harness can be long; the build line is in it. Read
# until the first line that is not part of a comment, with a hard cap.
HEADER_MAX_LINES = 200
_LEADER = re.compile(r"^\s*(?:/\*+|\*+/?|//)?\s?")

# Git's bash, not whatever "bash" is first on PATH: on Windows that can be
# WSL's, which sees a different filesystem and a different gcc.
BASH = next((b for b in (r"C:\Program Files\Git\bin\bash.exe",
                         r"C:\Program Files\Git\usr\bin\bash.exe")
             if os.path.exists(b)), None) or shutil.which("bash") or shutil.which("sh")


def _header(path):
    out = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for i, raw in enumerate(fh):
            if i >= HEADER_MAX_LINES:
                break
            s = raw.strip()
            if i > 0 and s and not (s.startswith(("*", "/*", "//")) or s.endswith("*/")):
                break            # first code line: the header is over
            out.append(_LEADER.sub("", raw.rstrip("\n")).strip())
    return out


def script_line(path):
    """`sh some/script.sh` when the header names a build script, else None.
    A script builds AND runs, so the caller runs it whole."""
    for line in _header(path):
        if re.match(r"^sh \S+\.sh\b", line):
            return line
    return None


def is_measurement(path):
    """True when the header says "Not a pass/fail harness": a tool that needs
    input (a WAV, a corpus) and prints numbers to compare, with no verdict.
    Run bare it prints its usage and exits non-zero, which a suite would
    otherwise report as a failure. The two OSD tools did, 2026-11-21."""
    return any("not a pass/fail harness" in l.lower() for l in _header(path))


def gcc_line(path):
    """The gcc/cc/clang command from the header, backslash continuations
    joined and any `&& ./run` or `; ./run` tail cut off. None if absent."""
    parts, collecting = [], False
    for line in _header(path):
        if not collecting and re.match(r"^(gcc|cc|clang)\b", line):
            collecting = True
        if collecting:
            cont = line.endswith("\\")
            parts.append(line[:-1].rstrip() if cont else line)
            if not cont:
                break
    if not parts:
        return None
    cmd = " ".join(parts)
    for cut in ("&&", ";"):
        cmd = cmd.split(cut)[0]
    return " ".join(cmd.split())


def run(cmd, cwd, **kw):
    """Run a header line as a POSIX shell would - brace lists included."""
    if not BASH:
        raise RuntimeError("no POSIX shell found (Git for Windows' bash)")
    return subprocess.run([BASH, "-c", cmd], cwd=cwd, capture_output=True,
                          text=True, **kw)
