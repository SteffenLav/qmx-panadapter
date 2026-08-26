#!/usr/bin/env python3
"""Measure our WSPR decoder against wsprd on identical audio, in dB.

    python tools/wspr_noise_ladder.py <reference.wav> [--steps 0,2,4,6,8,10] [--dial 14.0956]

METHOD. Raise a real recording's noise floor by a known number of dB and see what
each decoder still finds. Both get BYTE-IDENTICAL input, so the horizontal
distance between the two curves is our deficit in dB - the unit the WSPR world
states sensitivity in.

⛔ TWO THINGS THIS TOOL GOT WRONG FIRST TIME, both of which produced a
confident, plausible, WRONG number. They are why it now works the way it does:

  1. IT COUNTED OUR DECODES, NOT OUR REAL ONES. Under noise our decoder
     fabricates - at +8 and +10 dB it "found" N99NHI and MK7OLK where wsprd
     correctly found nothing. Comparing a partly-fabricated count against a real
     one is not a sensitivity measurement. Every decode of ours is now classified
     against wsprd's own output for the SAME file: CONFIRMED or FABRICATED, and
     only CONFIRMED counts toward sensitivity.

  2. IT BYPASSED THE GUARDS. wspr_cap_sweep called the decoder directly, so it
     measured the raw decoder rather than what the device would publish. It now
     runs with --guards (the REAL wspr_guard_check), and reports what the guards
     removed - both fabrications above were caught by SLOW.

  And before either: the regex demanded a 4-digit first field, but wsprd takes
  that field from the FILENAME. Temp files are named WSJT-style now. That bug
  silently discarded every wsprd decode and reported a deficit of ZERO.

⚠ Raising the floor by d dB lowers EVERY station's SNR by d dB, so the weakest
drop out first - the order IS the sensitivity ranking.
⚠ One fixed noise seed per step, so runs are comparable. Change --seed to check
a result is not an artefact of one draw.
"""
import sys, os, struct, math, random, subprocess, re

# The Windows console is cp1252. A non-ASCII character in a print() would
# abort the run AFTER all the expensive work - so make stdout tolerant.
try: sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception: pass

WSPRD = r"C:\WSJT\wsjtx\bin\wsprd.exe"
SWEEP = "./wspr_cap_sweep"

def read_wav(p):
    b = open(p, 'rb').read()
    assert b[:4] == b'RIFF' and b[8:12] == b'WAVE', "not RIFF/WAVE"
    n = struct.unpack('<I', b[40:44])[0] // 2
    return struct.unpack('<I', b[24:28])[0], list(struct.unpack('<%dh' % n, b[44:44+2*n])), b[:44]

def degrade(samples, d_db, seed):
    if d_db <= 0: return list(samples)
    p = sum(float(x)*x for x in samples) / len(samples)
    sigma = math.sqrt(p * (10 ** (d_db/10.0) - 1.0))
    rnd = random.Random(seed)
    out = []
    for x in samples:
        v = x + rnd.gauss(0.0, sigma)
        out.append(32767 if v > 32767 else (-32768 if v < -32768 else int(v)))
    return out

def wsprd_calls(path, dial):
    r = subprocess.run([WSPRD, '-f', dial, path], capture_output=True, text=True, timeout=900)
    # first field is wsprd's "time", taken from the FILENAME - never assume digits
    return set(re.findall(r'^\s*\S{4}\s+-?\d+\s+-?[\d.]+\s+[\d.]+\s+-?\d+\s+(\S+)',
                          r.stdout, re.M))

def our_calls(path, cands):
    """Returns (published, guarded_off) - one run, --guards enabled."""
    r = subprocess.run([SWEEP, path, str(cands), '--guards'],
                       capture_output=True, text=True, timeout=3600)
    pub  = set(re.findall(r"DECODED\s+'(\S+)'", r.stdout))
    gone = set(re.findall(r"GUARDED\s+'(\S+)'", r.stdout))
    return pub, gone

def main():
    if len(sys.argv) < 2: print(__doc__); return 1
    src   = sys.argv[1]
    steps = [0,2,4,6,8,10]
    dial  = "14.0956"
    seed  = 12345
    cands = 24
    for flag, cast in (('--steps', None), ('--dial', str), ('--seed', int), ('--cands', int)):
        if flag in sys.argv:
            v = sys.argv[sys.argv.index(flag)+1]
            if flag == '--steps': steps = [float(x) for x in v.split(',')]
            elif flag == '--dial': dial = v
            elif flag == '--seed': seed = int(v)
            else: cands = int(v)

    if not os.path.exists(SWEEP) and not os.path.exists(SWEEP + '.exe'):
        print(f"{SWEEP} not built. Build test/wspr_cap_sweep.c first."); return 1

    rate, samples, hdr = read_wav(src)
    print(f"{src}: {len(samples)} samples, {len(samples)/rate:.1f} s, dial {dial} MHz, seed {seed}")
    print()
    print(f"{'noise':>7} | {'wsprd':>5} | {'ours OK':>7} | {'FABRICATED':>10} | {'guarded off':>11} | missed")
    print("-" * 78)
    rows = []
    for d in steps:
        tmp = f"scratchpad/260825_{int(d):02d}00.wav"
        os.makedirs('scratchpad', exist_ok=True)
        open(tmp, 'wb').write(hdr + struct.pack('<%dh' % len(samples),
                                                *degrade(samples, d, seed)))
        truth = wsprd_calls(tmp, dial)
        pub, gone = our_calls(tmp, cands)
        good = pub & truth
        fake = pub - truth
        rows.append((d, len(truth), len(good), len(fake), len(gone)))
        missed = len(truth - pub)
        print(f"{d:>4.0f} dB | {len(truth):>5} | {len(good):>7} | {len(fake):>10} | "
              f"{len(gone):>11} | {missed:>3}"
              + ("   FABRICATED: " + ",".join(sorted(fake)) if fake else ""))
        try: os.remove(tmp)
        except OSError: pass

    print()
    base = rows[0][2]                      # our CONFIRMED count on clean audio
    hit  = [d for d, nt, ng, nf, gg in rows if nt <= base]
    print(f"our confirmed count on clean audio: {base}   (wsprd: {rows[0][1]})")
    if hit:
        print(f"wsprd falls to {base} only at +{hit[0]:.0f} dB of added noise")
        print(f"=> SENSITIVITY DEFICIT approximately {hit[0]:.0f} dB")
    else:
        print(f"wsprd never fell that low within the ladder - deficit exceeds "
              f"{steps[-1]:.0f} dB")
    tot_fake = sum(r[3] for r in rows)
    print(f"fabrications that survived the guards, all steps: {tot_fake}")
    if tot_fake:
        print("  WARNING: any of these would be published as a reception report.")
    return 0

sys.exit(main())
