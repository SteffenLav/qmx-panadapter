#!/usr/bin/env python3
"""Turn a serial capture into a readable WSPR reception report.

    python tools/wspr_report.py <capture.txt> [--grid JO65] [--out report.md]

Reads the wspr_rx log lines a run produces and writes one document with the
run's shape, every station heard, and the evidence for calling any of them
fabricated.

ONE FILE IS NOT ONE RUN
    A standing capture accumulates every session appended end to end. The dev
    bench file held 112 capture sessions across 110 reboots and several
    different firmware builds, and this tool used to report all of it as a
    single run: 56 stations, 197 decodes, no hint that anything had been
    pooled. It now splits on boots and reports only the LAST one, saying how
    many it found. --all-boots restores the old behaviour deliberately.

WHY THE "HEARD" COUNT IS THE IMPORTANT COLUMN
    A real station transmits again. A false decode does not. So repetition is
    ground truth that needs no second decoder and no internet - and on the
    2026-08-24 overnight run it separated the population cleanly: of 75 calls
    heard more than once, NONE declared over 40 dBm, while all 10 calls
    declaring 47-60 dBm were heard exactly once.
"""
import re, sys, math
from collections import defaultdict

DEC = re.compile(
    r"\((\d+)\)\s+wspr_rx:\s+DECODED\s+'([A-Z0-9/]+)'\s+'([A-Z0-9]+)'\s+(-?\d+)\s+dBm"
    r"\s+f=([\d.]+)\s+Hz\s+dt=(-?[\d.]+)s\s+cycles=(\d+)"
    # agree= is optional so this tool still reads captures from before the
    # re-encode check existed - there are months of them and they stay useful.
    r"(?:\s+agree=[\d.]+/(-?[\d.]+))?")
CYC = re.compile(r"\((\d+)\)\s+wspr_rx:\s+cycle\s+(\d+):\s+(\d+)\s+candidate\(s\),\s+(\d+)\s+decode")
ARM = re.compile(r"\((\d+)\)\s+wspr_rx:\s+cycle\s+(\d+):\s+capturing")
LEGAL = {0,3,7,10,13,17,20,23,27,30,33,37,40,43,47,50,53,57,60}

def grid_ll(g):
    """Maidenhead -> (lat, lon). 4 or 6 characters."""
    g = g.upper()
    if len(g) < 4: return None
    try:
        lon = (ord(g[0])-65)*20 - 180 + (ord(g[2])-48)*2
        lat = (ord(g[1])-65)*10 -  90 + (ord(g[3])-48)*1
    except Exception:
        return None
    if len(g) >= 6:
        lon += (ord(g[4])-65)*(2/24.0); lat += (ord(g[5])-65)*(1/24.0)
        lon += 1/24.0; lat += 0.5/24.0
    else:
        lon += 1.0; lat += 0.5
    return lat, lon

def km_brg(a, b):
    if not a or not b: return None, None
    la1,lo1 = map(math.radians, a); la2,lo2 = map(math.radians, b)
    d = 2*6371.0*math.asin(math.sqrt(math.sin((la2-la1)/2)**2 +
        math.cos(la1)*math.cos(la2)*math.sin((lo2-lo1)/2)**2))
    y = math.sin(lo2-lo1)*math.cos(la2)
    x = math.cos(la1)*math.sin(la2)-math.sin(la1)*math.cos(la2)*math.cos(lo2-lo1)
    return d, (math.degrees(math.atan2(y,x))+360) % 360

ANSI = re.compile(r"\x1b\[[0-9;]*m")
UP = re.compile(r"^[IWE] \((\d+)\)", re.M)


def last_boot(raw):
    """Return (text of the final boot, number of boots seen).

    ⛔ DO NOT SPLIT ON 'Loaded app from partition'. That is what this function
    replaced, and it is the mistake CLAUDE.md warns about in its own
    boot-counting rule: A RESET DOES NOT ALWAYS PRINT THAT LINE. A standing
    capture file accumulates every session appended end to end - the dev bench
    file had 112 capture sessions across 109 reboots and many different
    firmware builds - and rfind() of that marker landed in the middle of the
    history, so the tool reported 56 stations and 197 decodes as though they
    were one run. Nothing in the output hinted that four firmware versions had
    been pooled.

    THE ROBUST TEST IS A BACKWARD JUMP IN THE UPTIME COLUMN. Uptime is
    monotonic within a boot and restarts near zero after any reset, printed or
    not. The threshold exists because lines from different tasks interleave by
    a few milliseconds; a real reboot drops by hundreds of thousands.
    """
    marks = [(m.start(), int(m.group(1))) for m in UP.finditer(raw)]
    if not marks:
        return raw, 1
    starts = [0]
    prev = marks[0][1]
    for pos, up in marks:
        if up < prev - 5000:
            starts.append(pos)
        prev = up
    # Trim a trailing fragment with no decodes at all (a capture reopened
    # seconds before the file was copied): reporting on it would say "no
    # stations" about a receiver that is working perfectly.
    seg = raw[starts[-1]:]
    if len(starts) > 1 and not DEC.search(seg):
        seg = raw[starts[-2]:]
    return seg, len(starts)


def main():
    if len(sys.argv) < 2: print(__doc__); return 1
    path = sys.argv[1]
    my   = sys.argv[sys.argv.index('--grid')+1] if '--grid' in sys.argv else 'JO65'
    out  = sys.argv[sys.argv.index('--out')+1]  if '--out'  in sys.argv else 'wspr-report.md'
    home = grid_ll(my)

    raw = open(path, 'rb').read().decode('utf-8', 'replace')
    # ⛔ STRIP ANSI FIRST. Every line the device emits is wrapped in colour
    # codes, so a line does NOT begin with its severity letter and any regex
    # anchored with ^ silently matches nothing. That is not hypothetical: the
    # first version of last_boot() anchored on ^[IWE], found zero uptimes,
    # concluded there was one boot, and pooled 112 sessions - the exact failure
    # it was written to prevent, reported as 255 stations.
    raw = ANSI.sub('', raw)
    raw, nseg = last_boot(raw)
    if nseg > 1:
        print("note: %d boot segment(s) in this file - reporting only the LAST."
              % nseg)
        print("      Pass --all-boots to pool them, but read the docstring first.")
    if '--all-boots' in sys.argv:
        raw = open(path, 'rb').read().decode('utf-8', 'replace')
        print("note: --all-boots - POOLING every boot in this file. Different "
              "firmware builds may be mixed.")

    st = defaultdict(lambda: {'n':0,'pwr':None,'grid':None,'hz':[],'cyc':[],'up':[]})
    for m in DEC.finditer(raw):
        up, call, grid, pwr, hz, dt, cyc, agree = m.groups()
        s = st[call]
        s['n'] += 1; s['pwr'] = int(pwr); s['grid'] = grid
        s['hz'].append(float(hz)); s['cyc'].append(int(cyc)); s['up'].append(int(up))
        # None for captures predating the re-encode check; the column then
        # reads "-" rather than the tool refusing to run on old logs.
        if agree is not None: s.setdefault('agree', []).append(float(agree))

    cycles = CYC.findall(raw); arms = ARM.findall(raw)
    total  = sum(s['n'] for s in st.values())

    L = []
    L.append(f"# WSPR reception report\n")
    L.append(f"Capture: `{path}`  \nReceiver grid: **{my}**\n")
    L.append("## The run\n")
    if arms:
        span = (int(arms[-1][0]) - int(arms[0][0]))/3600000.0
        L.append(f"- Captures armed: **{len(arms)}**, spanning **{span:.1f} h**")
    if cycles:
        cand = [int(c[2]) for c in cycles]; dec = [int(c[3]) for c in cycles]
        L.append(f"- Cycles decoded: **{len(cycles)}**")
        L.append(f"- Candidates per cycle: {min(cand)}-{max(cand)}")
        L.append(f"- Decodes per cycle: mean **{sum(dec)/len(dec):.1f}**, best {max(dec)}")
    L.append(f"- Total decodes: **{total}**, unique callsigns: **{len(st)}**\n")

    rep  = {c:s for c,s in st.items() if s['n'] > 1}
    once = {c:s for c,s in st.items() if s['n'] == 1}
    L.append("## How many are real?\n")
    L.append("A real station transmits again; a false decode does not. Repetition is")
    L.append("therefore ground truth needing no second decoder.\n")
    L.append(f"- Heard **more than once** (real): **{len(rep)}**")
    L.append(f"- Heard **once only**: **{len(once)}**")
    if rep:
        L.append(f"- Highest power among the repeaters: **{max(s['pwr'] for s in rep.values())} dBm**\n")

    sus = []
    for c, s in st.items():
        why = []
        if c[0] in '01': why.append('callsign starts 0/1')
        if s['pwr'] not in LEGAL: why.append(f"illegal power {s['pwr']}")
        elif s['pwr'] > 43: why.append(f"{s['pwr']} dBm = {10**((s['pwr']-30)/10):.0f} W")
        if s['n'] == 1 and why: sus.append((c, s, '; '.join(why)))
    if sus:
        L.append("## Suspected fabrications\n")
        L.append("Implausible **and** heard only once - both signals agreeing.\n")
        L.append("| call | grid | power | heard | Fano cycles | why |")
        L.append("|---|---|---|---|---|---|")
        for c, s, why in sorted(sus, key=lambda x:-x[1]['pwr']):
            L.append(f"| {c} | {s['grid']} | {s['pwr']} dBm | {s['n']}x | {s['cyc'][0]} | {why} |")
        L.append("")

    L.append("## Every station heard\n")
    L.append("| call | grid | country-ish | km | brg | power | heard | Hz | Fano | agree |")
    L.append("|---|---|---|---|---|---|---|---|---|---|")
    for c, s in sorted(st.items(), key=lambda kv: (-kv[1]['n'], kv[0])):
        d, br = km_brg(home, grid_ll(s['grid'] or ''))
        hz = sum(s['hz'])/len(s['hz'])
        cy = f"{min(s['cyc'])}-{max(s['cyc'])}" if s['n'] > 1 else str(s['cyc'][0])
        flag = " ⚠" if any(c == x[0] for x in sus) else ""
        # The re-encode agreement score (wspr_decode.h). This is the column to
        # read when a station looks wrong: it is the only number in the row
        # that was measured against the received AUDIO rather than derived
        # from the message. "-" means a capture from before the check existed.
        ag = s.get('agree')
        ags = f"{min(ag):.2f}" if ag else "-"
        L.append(f"| {c}{flag} | {s['grid']} | {(s['grid'] or '')[:2]} | "
                 f"{d:.0f} | {br:.0f}° | {s['pwr']} dBm | {s['n']}x | {hz:.1f} | {cy} | {ags} |"
                 if d is not None else
                 f"| {c}{flag} | {s['grid']} | ? | ? | ? | {s['pwr']} dBm | {s['n']}x | {hz:.1f} | {cy} | {ags} |")
    L.append("\n⚠ = suspected fabrication, see above.\n")

    open(out, 'w', encoding='utf-8').write('\n'.join(L))
    print(f"wrote {out}: {len(st)} stations, {total} decodes, {len(sus)} suspect")
    return 0

sys.exit(main())
