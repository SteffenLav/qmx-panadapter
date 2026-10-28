#!/usr/bin/env python3
"""Which operator settings are read ONLY at boot? (T28)

    python tools/boot_only_settings.py .

⛔ THIS IS A LEAD GENERATOR, NOT A GATE. It exits 0 whatever it finds, and
every hit has to be READ before it is believed - the same rule T27's
70-of-159 sweep carries. Run on 2026-10-20 it reported 23 candidates and all
23 were false: each one is a value RESTORED at boot whose control also applies
it live, which is the opposite of the fault being looked for.

WHY IT EXISTS. RX audio takes effect only after a restart, for a correct
reason - the codec claims its DMA memory at boot or never - and for a while
said so only on the serial console. That raised the question of how many
others there are. The answer, today, is NONE: rx_audio toasts, and the BLE
mouse (the other boot-gated one) has a "Restart now" button on the Tab5 and a
`pending` flag in /api/status. A sentence saying that would rot; this can be
re-run.

HOW IT DECIDES, AND WHERE IT IS WEAK. A setting is consumed two ways here:
settings_load_all(&cfg) then cfg.field, and the accessor settings_get_field().
Both are counted - counting only the first found ONE field in 168, which is
how a sweep lies to you. The init test is then a GUESS at the function's name,
and it cannot see the common safe pattern (the control writes a live variable
and persists the setting separately), which is why it over-reports.

⚠ IT MUTATION-TESTED BADLY THE FIRST TIME. The original matcher had a
lookbehind before the dot, which silently excluded every NESTED access such as
`cfg.ft8_filters.excl_plain_cq` - 119 live settings read as "never consumed"
and the real question went unanswered. The check that caught it was running it
against a setting whose answer was already known (rx_audio_en). Do that to any
future change here.

Usage: boot_only_settings.py <repo root>
"""
import collections, os, re, sys

root = sys.argv[1].replace("\\", "/")


hdr = open(root + "/main/storage/settings.h", encoding="utf-8").read()
m = re.search(r"typedef struct\s*\{(.*?)\}\s*qmx_settings_t\s*;", hdr, re.S)
fields = []
for line in m.group(1).split("\n"):
    line = line.split("//")[0].split("/*")[0]
    mm = re.match(r"\s*(?:const\s+)?[A-Za-z_][A-Za-z0-9_ \*]*?\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^;]*\])?\s*;", line)
    if mm:
        fields.append(mm.group(1))
fields = sorted(set(fields))

# settings.c defines the accessors; config_io and webserver serialise every
# field by name, so all three would make every setting look widely read.
SKIP = ("main/storage/settings.c", "main/storage/config_io.c", "main/net/webserver.c")
srcs = []
for dirpath, _dirs, files in os.walk(root + "/main"):
    for f in files:
        if not f.endswith(".c"):
            continue
        p = (dirpath + "/" + f).replace("\\", "/")
        rel = p[len(root) + 1:]
        if rel in SKIP:
            continue
        srcs.append((rel, open(p, encoding="utf-8", errors="replace").read()))

FUNC = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \*]*?([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*$", re.M)

def enclosing(text, pos):
    best = "?"
    for mm in FUNC.finditer(text, 0, pos):
        best = mm.group(1)
    return best

sites = collections.defaultdict(list)
for fld in fields:
    # NO LOOKBEHIND before the dot. The first cut had one, to avoid matching a
    # longer identifier - and it threw away every NESTED access, because the
    # character before the dot in `cfg.ft8_filters.excl_plain_cq` is a letter.
    # That made 119 live settings read as "never consumed". The trailing
    # boundary is what actually prevents a prefix match.
    pats = [re.compile(r"(?:\.|->)" + re.escape(fld) + r"(?![A-Za-z0-9_])"),
            re.compile(r"settings_get_" + re.escape(fld) + r"\s*\(")]
    for rel, text in srcs:
        for pat in pats:
            for mm in pat.finditer(text):
                sites[fld].append((rel, text[:mm.start()].count("\n") + 1,
                                   enclosing(text, mm.start())))

INIT = re.compile(r"(^|_)(init|start|setup|boot|create|open|begin|install)($|_)|app_main")

boot = []
unread = []
for fld in fields:
    s = sites.get(fld, [])
    if not s:
        unread.append(fld)
        continue
    if all(INIT.search(fn) for _r, _l, fn in s):
        boot.append((fld, s))

print("=== read only inside init-looking functions (CANDIDATES) ===")
for fld, s in boot:
    print(fld)
    for rel, ln, fn in s:
        print("    %s:%d  %s()" % (rel, ln, fn))

print()
print("=== never read outside settings.c/config_io.c/webserver.c ===")
print(", ".join(unread) if unread else "(none)")

print()
print("%d declared, %d boot-only candidate(s), %d never consumed"
      % (len(fields), len(boot), len(unread)))
