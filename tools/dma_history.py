#!/usr/bin/env python3
"""Settled DMA/internal pool per BOOT from a bench capture.

⛔ Never `grep "T+600s" capture | tail -1`. The capture is one file across
many boots and flashes, so the newest such line usually belongs to the
PREVIOUS build - that mistake turned a working fix into "did not work"
on 2026-11-13. This splits on `memledger: app_main entry` first and reports
the firmware version from the SAME boot.

  python tools/dma_history.py scratchpad/capture-bench1.txt [--all]
"""
import re, sys

RE_FW = re.compile(rb'tab5_fw=(v[^ ]+) ')
RE_LED = re.compile(rb'memledger: (T\+\d+s settled|app_main entry|app_main exit)\s+dma=\s*(\d+) B \(lblk\s*(\d+)\)')

def boots(path):
    out, cur = [], None
    with open(path, 'rb') as f:
        for line in f:
            if b'app_main entry' in line and b'memledger' in line:
                cur = {'fw': '?', 'rows': {}}
                out.append(cur)
            if cur is None:
                continue
            if b'tab5_fw=' in line:
                m = RE_FW.search(line)
                if m:
                    cur['fw'] = m.group(1).decode()
            m = RE_LED.search(line)
            if m:
                cur['rows'][m.group(1).decode()] = (int(m.group(2)), int(m.group(3)))
    return out

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    path = args[0] if args else 'scratchpad/capture-bench1.txt'
    show_all = '--all' in sys.argv
    bs = boots(path)
    if not show_all:
        bs = bs[-12:]
    print(f"{'firmware':34s} {'entry':>8s} {'exit':>8s} {'T+22s':>8s} {'T+120s':>8s} {'T+600s':>8s}  lblk@600")
    for b in bs:
        g = lambda k: b['rows'].get(k, (None, None))
        v = [g('app_main entry')[0], g('app_main exit')[0], g('T+22s settled')[0],
             g('T+120s settled')[0], g('T+600s settled')[0]]
        lb = g('T+600s settled')[1]
        cells = ' '.join(f"{x:8d}" if x is not None else f"{'-':>8s}" for x in v)
        print(f"{b['fw']:34s} {cells}  {lb if lb is not None else '-'}")

if __name__ == '__main__':
    main()
