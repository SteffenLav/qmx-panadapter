"""T133: is the primary stft_ms bimodality the two-monitor pool alternating?

MEASURED on bench1 2026-10-30: stft_ms is bimodal, ~905 ms and ~1915 ms.
FT8_NUM_BUFFERS is 2 and find_free_buffer() scans from index 0, so while a
decode is still running when the next capture arms the pool alternates
0,1,0,1 - and a per-monitor cost difference then shows up as a STRICT slot-by-
slot alternation of stft_ms. If the high/low ran in blocks instead, the pool
would not be the cause.

⛔ THAT TEST IS AN INFERENCE, and it is only needed on captures from before
the firmware printed `mon=` on the slot line. Where mon= is present this reads
it directly and says so, because an inference that agrees with a measurement
is still an inference.

Named groups throughout - see the note in js8_alt_cost.py about the index
shift that adding mon= caused.

Usage:  python tools/js8_monitor_bimodal.py <capture.txt> [split_utc]
"""
import re, statistics, sys

P = re.compile(
    r"slot (?P<slot>\d+) UTC (?P<utc>\d+): (?:mon=(?P<mon>\d+) )?"
    r"off=(?P<off>[-+]\d+)ms cap=(?P<cap>\d+)ms stft=(?P<stft>\d+)ms "
    r"dec=(?P<dec>\d+)ms cand=(?P<cand>\d+)")
PLACE = re.compile(
    r"monplace (?P<mon>\d+): last_frame (?P<lf>0x[0-9a-f]+) (?P<lfw>\w+) \| "
    r"window (?P<win>0x[0-9a-f]+) (?P<winw>\w+) \| wf\.mag (?P<mag>0x[0-9a-f]+) (?P<magw>\w+)")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
HIGH_MS = 1400          # the trough between the two clusters


def mutation_test():
    good = ("I (1) ft8_test: slot 322 UTC 1793387145: mon=1 off=+1ms cap=13371ms "
            "stft=1930ms dec=1813ms cand=140 dec=0")
    m = P.search(good)
    assert m and m.group("stft") == "1930" and m.group("mon") == "1" \
        and m.group("cap") == "13371", f"wrong columns: {m.groupdict() if m else None}"
    old = good.replace("mon=1 ", "")
    m = P.search(old)
    assert m and m.group("stft") == "1930" and m.group("mon") is None, \
        f"pre-T133 line misread: {m.groupdict() if m else None}"
    pl = ("I (1) ft8_test: monplace 0: last_frame 0x4ff2a000 INTERNAL | window "
          "0x48123000 PSRAM | wf.mag 0x48200000 PSRAM | timedata 0x1 | freqdata 0x2 "
          "| fft_work 0x3")
    m = PLACE.search(pl)
    assert m and m.group("lfw") == "INTERNAL" and m.group("winw") == "PSRAM", \
        f"monplace misread: {m.groupdict() if m else None}"
    assert not PLACE.search(good) and not P.search(pl), "the two regexes cross-match"
    print("regex mutation test: slot line (both formats) and monplace read "
          "correctly, no cross-match")


def main(path, split=None):
    mutation_test()
    rows, place = [], []
    with open(path, "rb") as f:
        for raw in f:
            line = ANSI.sub("", raw.decode("utf-8", "replace").replace("\0", ""))
            m = PLACE.search(line)
            if m: place.append(m.groupdict()); continue
            m = P.search(line)
            if m: rows.append(m.groupdict())

    if place:
        print("\nWHERE EACH MONITOR'S PRIVATE BUFFERS LANDED (newest build):")
        seen = {}
        for q in place: seen[q["mon"]] = q
        for k in sorted(seen):
            q = seen[k]
            print(f"  mon {k}: last_frame {q['lf']} {q['lfw']:<8} "
                  f"window {q['win']} {q['winw']:<8} wf.mag {q['mag']} {q['magw']}")
    else:
        print("\nno `monplace` lines - this capture predates the T133 diagnostic")

    # Direct read where the firmware reports the index.
    direct = [r for r in rows if r["mon"] is not None]
    if direct:
        print(f"\nstft_ms BY POOL INDEX, read from the slot line (n={len(direct)}):")
        by = {}
        for r in direct: by.setdefault(r["mon"], []).append(int(r["stft"]))
        for k in sorted(by):
            v = sorted(by[k])
            print(f"  mon {k}: n={len(v):<5} p50={statistics.median(v):7.0f} "
                  f"min={v[0]} max={v[-1]}")
        if len(by) == 2:
            a, b = (statistics.median(by[k]) for k in sorted(by))
            print(f"  ratio slow/fast = {max(a,b)/max(1.0,min(a,b)):.2f}x")

    # The inference, for captures with no mon= field.
    legacy = [r for r in rows if r["mon"] is None]
    if legacy:
        windows = [("ALL", lambda u: True)] if split is None else [
            ("BEFORE", lambda u: u < split), ("AFTER", lambda u: u >= split)]
        for name, sel in windows:
            s = [r for r in legacy if sel(int(r["utc"]))]
            if len(s) < 4: continue
            seq = "".join("H" if int(r["stft"]) > HIGH_MS else "l" for r in s)
            flips = sum(1 for i in range(1, len(seq)) if seq[i] != seq[i - 1])
            runs, cur = [], 1
            for i in range(1, len(seq)):
                if seq[i] == seq[i - 1]: cur += 1
                else: runs.append(cur); cur = 1
            runs.append(cur)
            print(f"\n{name} (inferred, no mon= field)  n={len(s)}")
            print(f"  sequence : {seq[:72]}")
            print(f"  flips {flips} of {len(seq)-1} ({100.0*flips/max(1,len(seq)-1):.0f}%"
                  f" - 100% is strict alternation)")
            print(f"  run lengths: max {max(runs)}, mean {statistics.mean(runs):.2f}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else None)
