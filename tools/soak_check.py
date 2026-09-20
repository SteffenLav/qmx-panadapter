# Morning read-out for the 2026-09-11 overnight soak.
#   python scratchpad/soak-check.py [capture file]
#
# Testing three unreleased fixes:
#   patch #18  rpc_core.c  - the orphaned-response deadlock in the 3-deep rpc_rx_q
#   patch #16/#17          - esp_hosted sendbuf drop + task stacks
#   1f374a0                - display_lock() priority inversion (USB audio loss)
#
# THE VERDICTS
#   #18 CONFIRMED   : "no waiter ... dropping" lines present AND no timeout storm
#   #18 not exercised: no such lines at all (the seed needs a memory-pressure spike)
#   #18 FAILED      : a run of Timeout-waiting lines at ~1 per 5 s that never stops
#   audio OK        : WSPR keeps decoding; rate stays ~12.000 smp/ms
import re, sys, os, collections

path = sys.argv[1] if len(sys.argv) > 1 else "scratchpad/capture-dev.txt"
if not os.path.isfile(path):
    sys.exit("no such capture: " + path)

lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
print("file: %s  (%.1f MB, %d lines)" % (path, os.path.getsize(path) / 1e6, len(lines)))

boots = [i for i, l in enumerate(lines) if "Loaded app from partition" in l]
print("boots: %d" % len(boots))
for i, l in enumerate(lines):
    if "THE PREVIOUS BOOT CRASHED" in l or "assert failed" in l or "Guru Meditation" in l:
        print("  !! line %d: %s" % (i + 1, l.strip()[:160]))

pats = {
    "#18 no-waiter drop":  "no waiter for resp",
    "#18 no-sem refuse":   "no sem for req",
    "RPC timeout":         "Timeout waiting for Resp",
    "RPC resp-not-recvd":  "Response not received",
    "sem create failed":   "sem create failed",
    "SDIO q backed up":    "task still writing Rx data to queue",
    "SDIO rx alloc fail":  "SDIO RX buffer alloc failed",
    "SDIO oversize drain": "SDIO RX oversize",
    "ring full (audio)":   "ring full",
}
times = {k: [] for k in pats}
for l in lines:
    m = re.search(r"\((\d+)\)", l)
    if not m:
        continue
    t = int(m.group(1))
    for k, p in pats.items():
        if p in l:
            times[k].append(t)

print("\nevent                 count   first      last")
for k in pats:
    ts = times[k]
    if ts:
        print("%-20s %6d  %7.1fs  %7.1fs" % (k, len(ts), ts[0] / 1000, ts[-1] / 1000))
    else:
        print("%-20s %6d" % (k, 0))

# A wedge is a SUSTAINED timeout run, not the odd one. Report the longest stretch
# of consecutive timeouts spaced under 15 s - that is the signature to fear.
ts = sorted(times["RPC timeout"])
run = best = 0
start = bstart = 0
for i, t in enumerate(ts):
    if i and t - ts[i - 1] < 15000:
        run += 1
    else:
        run, start = 1, t
    if run > best:
        best, bstart = run, start
if ts:
    print("\nlongest unbroken RPC-timeout run: %d, starting %.1fs" % (best, bstart / 1000))
    if best >= 10:
        print("  ^^ THAT IS A WEDGE - patch #18 did not hold, or there is a third cause")
else:
    print("\nno RPC timeouts at all")

if times["#18 no-waiter drop"] or times["#18 no-sem refuse"]:
    print("\n*** patch #18 FIRED - the orphan path was actually exercised ***")
    for l in lines:
        if "no waiter for resp" in l or "no sem for req" in l:
            print("  " + l.strip()[:160])
else:
    print("\npatch #18 never fired - not exercised, so not confirmed either way")

# heap trend: both log shapes (normal, and the LOW! emergency one)
heap = []
for l in lines:
    m = re.search(r"\((\d+)\) audio: HEAP: int free=(\d+)KB \(min=(\d+)KB[^)]*\).*?psram free=(\d+)KB", l)
    if m:
        heap.append(tuple(int(x) for x in m.groups()))
if heap:
    print("\nheap, every ~30th sample (int KB / psram KB):")
    for i in range(0, len(heap), max(1, len(heap) // 20)):
        ms, f, mn, ps = heap[i]
        print("  t=%8.1fs  int=%3d (min %2d)  psram=%6d" % (ms / 1000, f, mn, ps))
    ms, f, mn, ps = heap[-1]
    print("  t=%8.1fs  int=%3d (min %2d)  psram=%6d   <- last" % (ms / 1000, f, mn, ps))

# WSPR decode yield is the direct read on audio integrity
cyc = [l for l in lines if "wspr_rx: cycle" in l and "decode(s)" in l]
if cyc:
    dec = [int(re.search(r"(\d+) decode\(s\)", c).group(1)) for c in cyc]
    print("\nWSPR cycles: %d, decodes total %d, zero-decode cycles %d"
          % (len(cyc), sum(dec), sum(1 for d in dec if d == 0)))
    print("  last 8: " + " ".join(str(d) for d in dec[-8:]))

# ⛔ SPLIT BY BOOT BEFORE JUDGING THE RATE. The SD mirror is a ROLLING file that
# spans many boots, so a flat scan mixes the run you care about with whatever
# short boots preceded it - and on 2026-09-12 that made me report "9 of 47 WSPR
# cycles lost audio" when every one of the nine was an FT8 arm from a 7-minute
# boot the day before, some of it on firmware predating the gap-fill fix (those
# lines have no "filled=" field). The overnight run itself was clean.
# Also drop rate=0.000: that is the first arm of a boot, with no previous sample
# to measure against, not a cycle that lost everything.
arm = re.compile(r"\((\d+)\) dsp: FT8 arm:.*?rate=([\d.]+) smp/ms(.*)$")
rows = []
for l in lines:
    m = arm.search(l)
    if m:
        rows.append((int(m.group(1)), float(m.group(2)), "filled=" in m.group(3)))

if rows:
    runs = [[]]
    for r in rows:
        if runs[-1] and r[0] < runs[-1][-1][0]:
            runs.append([])          # uptime went backwards: a new boot
        runs[-1].append(r)

    print("\nsample rate per boot (12.000 = nothing lost; 0.000 first arm ignored):")
    print("  %-5s %-9s %-9s %5s %5s %8s %8s" % ("boot", "from", "to", "n", "lost", "min", "filled?"))
    for i, b in enumerate(runs):
        r = [x[1] for x in b if x[1] > 0]
        if not r:
            continue
        lost = [x for x in r if x < 11.97]
        print("  %-5d %-9.0f %-9.0f %5d %5d %8.3f %8s" % (
            i + 1, b[0][0] / 1000, b[-1][0] / 1000, len(r), len(lost), min(r),
            "yes" if b[-1][2] else "NO"))
    last = [x[1] for x in runs[-1] if x[1] > 0]
    if last:
        lost = [x for x in last if x < 11.97]
        verdict = ("%d of %d cycles lost audio at the wire" % (len(lost), len(last))
                   if lost else "no audio lost")
        print("  NEWEST boot: %s (min %.3f)" % (verdict, min(last)))
