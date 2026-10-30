"""Is the primary stft_ms bimodality the two-monitor pool alternating?

FT8_NUM_BUFFERS is 2 and find_free_buffer() scans from index 0, so while a
decode is still running when the next capture arms, the pool alternates
0,1,0,1 - and a per-monitor cost difference would then show up as a strict
slot-by-slot alternation of stft_ms. If instead the high/low runs in blocks,
the pool is not the cause and something else is.

Cheapest decisive test available: it needs no build and no bench time, only
the capture already on disk.
"""
import re, statistics, sys
P = re.compile(r"slot (\d+) UTC (\d+): off=([-+]\d+)ms cap=(\d+)ms stft=(\d+)ms "
               r"dec=(\d+)ms cand=(\d+)")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
SPLIT = 1793387400

rows = []
with open(sys.argv[1], "rb") as f:
    for raw in f:
        m = P.search(ANSI.sub("", raw.decode("utf-8", "replace").replace("\0", "")))
        if m and int(m.group(2)) >= 1793386000:
            rows.append((int(m.group(2)), int(m.group(1)), int(m.group(5)),
                         int(m.group(6)), int(m.group(4))))

for name, sel in (("BEFORE", lambda u: u < SPLIT), ("AFTER", lambda u: u >= SPLIT)):
    s = [r for r in rows if sel(r[0])]
    if len(s) < 4: continue
    seq = "".join("H" if r[2] > 1400 else "l" for r in s)
    # run lengths - a strict alternation gives every run length 1
    runs, cur = [], 1
    for i in range(1, len(seq)):
        if seq[i] == seq[i-1]: cur += 1
        else: runs.append(cur); cur = 1
    runs.append(cur)
    alt = sum(1 for i in range(1, len(seq)) if seq[i] != seq[i-1])
    print(f"\n{name}  n={len(s)}")
    print(f"  sequence : {seq[:72]}")
    print(f"  flips {alt} of {len(seq)-1} adjacent pairs "
          f"({100.0*alt/max(1,len(seq)-1):.0f}% - 100% is strict alternation)")
    print(f"  run lengths: max {max(runs)}, mean {statistics.mean(runs):.2f}")
    # does cap_ms track it? a shorter capture leaves more blocks for the tail
    hi = [r[4] for r in s if r[2] > 1400]; lo = [r[4] for r in s if r[2] <= 1400]
    if hi and lo:
        print(f"  cap_ms  with HIGH stft p50 {statistics.median(hi):.0f}, "
              f"with LOW p50 {statistics.median(lo):.0f}")
