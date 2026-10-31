"""Pull the T132 alternate-submode cost out of a bench capture.

Two separate series, deliberately: the ordinary slot line (the PRIMARY decode,
the instrument 6ace379c measured the budget with) and `altslot` (the alternate
window). They must not be mixed - a second line in the slot format would have
halved the apparent median of the first.

⛔ NAMED GROUPS, NOT NUMBERED. The first cut used numbered groups, and adding
the optional `mon=` field for T133 shifted every index by one while every
regex still matched - the scraper would have reported cap_ms as stft_ms with
no error anywhere. A capture scraper that silently reads the wrong column is
the same class of failure as a decoder fed the wrong window: it produces a
plausible number, not a complaint.

The regexes are MUTATION-TESTED below before they are trusted on 800 MB of
capture, the same discipline 6ace379c used.

Usage:  python tools/js8_alt_cost.py <capture.txt> [since_utc]
"""
import re, sys, statistics

PRIMARY = re.compile(
    r"slot (?P<slot>\d+) UTC (?P<utc>\d+): (?:mon=(?P<mon>\d+) )?"
    r"off=(?P<off>[-+]\d+)ms cap=(?P<cap>\d+)ms stft=(?P<stft>\d+)ms "
    r"dec=(?P<dec>\d+)ms cand=(?P<cand>\d+) dec=(?P<decoded>\d+)")
ALT = re.compile(
    r"altslot (?P<slot>\d+) UTC (?P<utc>\d+) (?P<proto>JS8 \w+): "
    r"stft=(?P<stft>\d+)ms dec=(?P<dec>\d+)ms cand=(?P<cand>\d+) "
    r"dec=(?P<decoded>\d+) js8f=(?P<js8f>\d+)")
ANSI = re.compile(r"\x1b\[[0-9;]*m")

GOOD_P_OLD = ("I (220477) ft8_test: slot 322 UTC 1793387145: off=+1ms cap=13371ms "
              "stft=1930ms dec=1813ms cand=140 dec=0 js8f=0 dup=0 skip=0")
GOOD_P_NEW = ("I (220477) ft8_test: slot 322 UTC 1793387145: mon=1 off=+1ms cap=13371ms "
              "stft=1930ms dec=1813ms cand=140 dec=0 js8f=0 dup=0 skip=0")
GOOD_A = ("I (220477) ft8_test: altslot 4 UTC 1793387400 JS8 Fast: stft=555ms "
          "dec=1671ms cand=140 dec=0 js8f=0 dup=0 skip=0")


def mutation_test():
    for name, line in (("pre-T133 slot", GOOD_P_OLD), ("post-T133 slot", GOOD_P_NEW)):
        m = PRIMARY.search(line)
        assert m, f"primary regex misses a real {name} line"
        # ⛔ THE COLUMNS, NOT JUST THE MATCH. This is the check that would have
        # caught the index shift that adding mon= caused.
        assert m.group("cap") == "13371" and m.group("stft") == "1930" \
            and m.group("dec") == "1813" and m.group("cand") == "140", \
            f"primary regex read the wrong columns on the {name} line: {m.groupdict()}"
    assert PRIMARY.search(GOOD_P_NEW).group("mon") == "1", "mon= not read"
    assert PRIMARY.search(GOOD_P_OLD).group("mon") is None, "mon= invented"
    m = ALT.search(GOOD_A)
    assert m and m.group("stft") == "555" and m.group("dec") == "1671" \
        and m.group("proto") == "JS8 Fast", f"alt regex columns: {m.groupdict() if m else None}"
    # ⛔ THE ONE THAT MATTERS: neither may match the other's line, or the
    # alternate's cost folds into its own baseline and the comparison is void.
    assert not ALT.search(GOOD_P_NEW), "alt regex matched a PRIMARY slot line"
    assert not PRIMARY.search(GOOD_A), "primary regex matched an altslot line"
    bad = [GOOD_A.replace("stft=555ms", "stft=ms"),
           GOOD_A.replace("dec=1671ms", "dec=-1671ms"),
           GOOD_A.replace("altslot 4", "altslot"),
           GOOD_A.replace("JS8 Fast:", ":")]
    for i, b in enumerate(bad):
        assert not ALT.search(b), f"alt regex accepted corruption {i}: {b}"
    assert not PRIMARY.search(GOOD_P_NEW.replace("cap=13371ms", "cap=13371"))
    print("regex mutation test: 3 good lines accepted with the right columns, "
          "2 cross-matches rejected, 5 corruptions rejected")


def stats(name, vals):
    if not vals:
        print(f"  {name}: NO SAMPLES"); return None
    v = sorted(vals)
    p = lambda q: v[min(len(v) - 1, int(q * len(v)))]
    print(f"  {name:<18} n={len(v):<6} p50={statistics.median(v):<8.0f} "
          f"p90={p(0.90):<8.0f} p99={p(0.99):<8.0f} max={v[-1]}")
    return statistics.median(v)


def main(path, since=0):
    mutation_test()
    prim, alt = [], []
    with open(path, "rb") as f:
        for raw in f:
            line = ANSI.sub("", raw.decode("utf-8", "replace").replace("\0", ""))
            m = ALT.search(line)
            if m:
                if int(m.group("utc")) >= since: alt.append(m.groupdict())
                continue
            m = PRIMARY.search(line)
            if m and int(m.group("utc")) >= since: prim.append(m.groupdict())

    print(f"\nslots since UTC {since}: primary {len(prim)}, alternate {len(alt)}")
    print("\nPRIMARY (15 s slot):")
    stats("stft_ms", [int(x["stft"]) for x in prim])
    stats("dec_ms", [int(x["dec"]) for x in prim])
    pm = stats("stft+dec", [int(x["stft"]) + int(x["dec"]) for x in prim])

    # T133: split by pool index when the firmware reports it.
    by_mon = {}
    for x in prim:
        if x["mon"] is not None:
            by_mon.setdefault(x["mon"], []).append(int(x["stft"]))
    if by_mon:
        print("\n  stft_ms BY POOL INDEX (T133):")
        for k in sorted(by_mon):
            stats(f"    mon {k}", by_mon[k])

    print("\nALTERNATE window:")
    if alt: print(f"  submodes seen: {sorted(set(x['proto'] for x in alt))}")
    stats("stft_ms", [int(x["stft"]) for x in alt])
    stats("dec_ms", [int(x["dec"]) for x in alt])
    am = stats("stft+dec", [int(x["stft"]) + int(x["dec"]) for x in alt])

    if pm and am:
        base, add = pm / 15000.0, am / (15000.0 * 5)
        print(f"\nCPU, one core, from these medians:")
        print(f"  primary alone          {base*100:5.1f}%")
        print(f"  + alternate at 1-in-5  {add*100:5.1f}%  added")
        print(f"  = total                {(base+add)*100:5.1f}%")
        phases = sorted(set((int(x['utc']) % 60) // 15 for x in alt))
        print(f"\n  phases of the minute visited: {phases} ({len(phases)} of 4 - "
              f"fewer than 4 means the duty cycle is phase-locked)")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 0)
