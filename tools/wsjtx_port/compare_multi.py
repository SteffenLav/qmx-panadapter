"""Port vs jt9 when BOTH ran over a sequence of slots in ONE process.

That is the only referee for a7 (previous-cycle) decodes and for hashes
resolved from earlier slots: jt9 on a single WAV has neither.

  jt9:  cd DIR; jt9 -8 -d 3 $(ls FT8_*.wav | sort) > jt9_multi.txt
  port: cd DIR; wsx_cmp.exe $(ls FT8_*.wav | sort) > port_multi.txt
  python tools/wsjtx_port/compare_multi.py jt9_multi.txt port_multi.txt

jt9 lines carry nutc (the 6 digits before .wav), the port's follow a
"FILE\t<path>" line; both are keyed by nutc. AP = any "aN" line (a1, a2, a7).
"""
import collections, re, sys

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.dirname(__import__("os").path.abspath(__file__))))
from ft8_vs_wsjtx import norm  # noqa: E402

jt9 = collections.defaultdict(lambda: [set(), set()])
kinds = collections.Counter()
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = re.match(r"^\s*(\d{6})\s+-?\d+\s+-?[\d.]+\s+\d+\s+~\s+(.*?)\s*$", l)
    if not m:
        continue
    txt = m[2]
    a = re.search(r"(\s+\?)?\s+(a\d)\s*$", txt)
    if a:
        kinds[a[2]] += 1
        jt9[int(m[1])][1].add(norm(txt[:a.start()]))
    else:
        jt9[int(m[1])][0].add(norm(txt))
port = collections.defaultdict(lambda: [set(), set()])
cur = None
for l in open(sys.argv[2], encoding="utf-8", errors="replace"):
    f = l.rstrip("\n").split("\t")
    if f[0] == "FILE":
        cur = int(re.search(r"(\d{6})\.wav", f[1])[1]); port[cur]; continue
    if len(f) >= 3 and cur is not None:
        port[cur][1 if f[0] == "APMSG" else 0].add(norm(f[2]))
keys = sorted(set(jt9) | set(port))
same = 0
sums = collections.Counter()
diffs = []
for k in keys:
    jp, ja = jt9[k]; pp, pa = port[k]
    sums["jt9 plain"] += len(jp); sums["port plain"] += len(pp); sums["both plain"] += len(jp & pp)
    sums["jt9 AP"] += len(ja); sums["port AP"] += len(pa); sums["both AP"] += len(ja & pa)
    if jp == pp and ja == pa:
        same += 1
    else:
        diffs.append((k, sorted(pp - jp), sorted(jp - pp), sorted(pa - ja), sorted(ja - pa)))
print(f"{len(keys)} slots, identical {same}; jt9 AP kinds {dict(kinds)}")
print("  " + ", ".join(f"{k} {v}" for k, v in sums.items()))
for k, a, b, c, d in diffs[:30]:
    print(f"  {k}: plain +{a} -{b}  AP +{c} -{d}")
