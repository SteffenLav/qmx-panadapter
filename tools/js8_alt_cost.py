"""Pull the T132 alternate-submode cost out of a bench capture.

Two separate series, deliberately: the ordinary slot line (the PRIMARY decode,
the instrument 6ace379c measured the budget with) and `altslot` (the alternate
window). They must not be mixed - a second line in the slot format would have
halved the apparent median of the first.

The regex is MUTATION-TESTED below before it is trusted on 800 MB of capture,
the same discipline 6ace379c used: a scraper that matched a corrupted line, or
silently matched nothing, reads exactly like a healthy quiet result.
"""
import re, sys, statistics

PRIMARY = re.compile(
    r"slot (\d+) UTC (\d+): off=([-+]\d+)ms cap=(\d+)ms stft=(\d+)ms dec=(\d+)ms "
    r"cand=(\d+) dec=(\d+)")
ALT = re.compile(
    r"altslot (\d+) UTC (\d+) (JS8 \w+): stft=(\d+)ms dec=(\d+)ms "
    r"cand=(\d+) dec=(\d+) js8f=(\d+)")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def mutation_test():
    good_p = ("I (220477) ft8_test: slot 322 UTC 1793387145: off=+1ms cap=13371ms "
              "stft=1930ms dec=1813ms cand=140 dec=0 js8f=0 dup=0 skip=0")
    good_a = ("I (220477) ft8_test: altslot 4 UTC 1793387400 JS8 Fast: stft=555ms "
              "dec=1671ms cand=140 dec=0 js8f=0 dup=0 skip=0")
    assert PRIMARY.search(good_p), "primary regex misses a real slot line"
    assert ALT.search(good_a), "alt regex misses a real altslot line"
    # ⛔ THE ONE THAT MATTERS: the two must not match each other's lines, or the
    # alternate's cost is folded into the baseline and the comparison is void.
    assert not ALT.search(good_p), "alt regex matched a PRIMARY slot line"
    assert not PRIMARY.search(good_a), "primary regex matched an altslot line"
    bad = [
        good_a.replace("stft=555ms", "stft=ms"),
        good_a.replace("dec=1671ms", "dec=-1671ms"),
        good_a.replace("altslot 4", "altslot"),
        good_a.replace("JS8 Fast:", ":"),
        good_p.replace("cap=13371ms", "cap=13371"),
    ]
    for i, b in enumerate(bad):
        assert not (ALT.search(b) and PRIMARY.search(b)), f"mutation {i} matched both"
        if i < 4:
            assert not ALT.search(b), f"alt regex accepted corruption {i}: {b}"
        else:
            assert not PRIMARY.search(b), f"primary regex accepted corruption {i}"
    print("regex mutation test: 2 good accepted, 2 cross-matches rejected, "
          "5 corruptions rejected")


def stats(name, vals):
    if not vals:
        print(f"  {name}: NO SAMPLES")
        return
    vals = sorted(vals)
    p = lambda q: vals[min(len(vals) - 1, int(q * len(vals)))]
    print(f"  {name:<18} n={len(vals):<6} p50={statistics.median(vals):<8.0f} "
          f"p90={p(0.90):<8.0f} p99={p(0.99):<8.0f} max={vals[-1]}")


def main(path, since_utc=0):
    mutation_test()
    prim, alt = [], []
    with open(path, "rb") as f:
        for raw in f:
            line = ANSI.sub("", raw.decode("utf-8", "replace").replace("\0", ""))
            m = ALT.search(line)
            if m:
                if int(m.group(2)) >= since_utc:
                    alt.append((int(m.group(2)), m.group(3), int(m.group(4)),
                                int(m.group(5)), int(m.group(6)), int(m.group(7))))
                continue
            m = PRIMARY.search(line)
            if m and int(m.group(2)) >= since_utc:
                prim.append((int(m.group(2)), int(m.group(4)), int(m.group(5)),
                             int(m.group(6)), int(m.group(7)), int(m.group(8))))

    print(f"\nslots since UTC {since_utc}: primary {len(prim)}, alternate {len(alt)}")
    print("\nPRIMARY (JS8 Normal, 15 s slot):")
    stats("stft_ms", [x[2] for x in prim])
    stats("dec_ms", [x[3] for x in prim])
    stats("stft+dec", [x[2] + x[3] for x in prim])
    print("\nALTERNATE window:")
    if alt:
        print(f"  submodes seen: {sorted(set(x[1] for x in alt))}")
    stats("stft_ms", [x[2] for x in alt])
    stats("dec_ms", [x[3] for x in alt])
    stats("stft+dec", [x[2] + x[3] for x in alt])

    if prim and alt:
        pm = statistics.median([x[2] + x[3] for x in prim])
        am = statistics.median([x[2] + x[3] for x in alt])
        base = pm / 15000.0
        add = am / (15000.0 * 5)          # one window per JS8_ALT_DUTY_EVERY slots
        print(f"\nCPU, one core, from these medians:")
        print(f"  primary alone        {base*100:5.1f}%")
        print(f"  + alternate at 1-in-5{add*100:5.1f}%  added")
        print(f"  = total              {(base+add)*100:5.1f}%   "
              f"(predicted 29%, the T132 row's original figure 48%)")
        # Phase coverage: the whole reason the duty is 5 and not 4.
        phases = sorted(set((x[0] % 60) // 15 for x in alt))
        print(f"\n  phases of the minute visited: {phases}  "
              f"({len(phases)} of 4 - fewer than 4 means the duty cycle is "
              f"phase-locked and most of the band is unreachable)")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 0)
