"""Mutation-test the decoded-CW grid harness.

    python test/mutate_cw_grid_rows.py <a-pristine-copy-of-cw_decode.c>

Breaks main/cw_decode.c one way at a time, rebuilds and runs
test/cw_grid_rows_harness.c, and restores the source afterwards. A mutation
that SURVIVES means the harness does not test what it claims to. See the list
at the bottom of the harness.

The argument is the pristine source, copied aside before the first run,
because every mutation is applied to a fresh copy of it.
"""
import subprocess
import sys
import shutil

SRC = "main/cw_decode.c"
ORIG = sys.argv[1]

MUTS = [
    ("1 wrap modulus back to the whole buffer",
     "            s_line_col = (s_line_col + 1) % grid_cells();",
     "            s_line_col = (s_line_col + 1) % CW_GRID_CELLS;"),
    ("2 cw_decode_line returns the whole buffer",
     "    size_t n = (size_t)grid_cells();",
     "    size_t n = (size_t)CW_GRID_CELLS;"),
    ("3 set_rows does not wipe",
     "    s_rows = rows;\n    memset(s_line, ' ', sizeof(s_line));\n    s_line_col = 0;",
     "    s_rows = rows;"),
    ("4 set_rows wipes even when unchanged",
     "    if (rows == s_rows) return;",
     "    if (rows == s_rows && 0) return;"),
    ("5 low clamp removed",
     "    if (rows < 1) rows = 1;",
     "    if (rows < -99) rows = 1;"),
    ("6 high clamp removed",
     "    if (rows > CW_LINE_ROWS) rows = CW_LINE_ROWS;",
     "    if (rows > 9999) rows = CW_LINE_ROWS;"),
]


def main():
    fails = 0
    for name, old, new in MUTS:
        shutil.copy(ORIG, SRC)
        s = open(SRC, encoding="utf-8").read()
        if old not in s:
            print(f"  ?? {name}: PATTERN NOT FOUND - mutation not applied")
            fails += 1
            continue
        open(SRC, "w", encoding="utf-8").write(s.replace(old, new, 1))
        r = subprocess.run(["sh", "test/build_cw_grid_rows.sh"],
                           capture_output=True, text=True)
        out = r.stdout + r.stderr
        if "FAILURES" in out:
            bad = [l.strip() for l in out.splitlines() if l.strip().startswith("FAIL")]
            print(f"  killed  {name}  <- {bad[0] if bad else ''}")
        elif r.returncode != 0:
            print(f"  killed  {name}  <- did not build/run (rc={r.returncode})")
        else:
            print(f"  SURVIVED {name}  *** harness does not catch this ***")
            fails += 1
    shutil.copy(ORIG, SRC)
    print("restored")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
