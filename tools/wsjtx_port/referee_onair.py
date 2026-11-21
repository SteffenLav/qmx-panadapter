"""Score bench1's ON-AIR piece decodes against jt9 on the same slots.

    python tools/wsjtx_port/referee_onair.py <capture.txt> <slot_wav_dir> [--boot VERSION]

The capture holds, per slot, the base (ft8_lib) decodes, then the slot line
("slot N UTC T: ..."), then the WSJT-X piece decodes and the per-slot
"slot N wsjtx pieces:" line. The WAV directory holds the same slots as
/api/slot.wav served them (tools/js8_corpus.py) - the very buffer both
decoders read - named yymmdd_hhmmss.wav from the slot's UTC.

jt9 (WS 3.2.1, -d 3) is the referee: a piece decode it also finds is
CONFIRMED, one it does not is UNCONFIRMED (a likely false decode, or a signal
only our candidate list reached - the count is the question, not the verdict).
Only slots with both a WAV and a pieces line are scored.
"""
import argparse
import collections
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import ft8_vs_wsjtx as V  # noqa: E402

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def hfold(t):
    return re.sub(r"<[^>]*>", "<H>", t)


def parse(capture, boot):
    slots = {}
    base, piece = [], []
    on = boot is None
    with open(capture, encoding="utf-8", errors="replace") as f:
        for raw in f:
            l = ANSI.sub("", raw)
            if "App version:" in l:
                on = boot is None or boot in l
                base, piece = [], []
                continue
            if not on:
                continue
            m = re.search(r"decoded: '(.*)' \(score=", l)
            if m:
                base.append(m[1]); continue
            m = re.search(r"decoded: '(.*)' \(wsjtx piece", l)
            if m:
                piece.append(m[1]); continue
            m = re.search(r"\bslot (\d+) UTC (\d+): ", l)
            if m:
                slots[int(m[1])] = {"utc": int(m[2]), "base": base, "piece": []}
                base = []; piece = []
                continue
            m = re.search(r"\bslot (\d+) wsjtx pieces: tried (\d+) of (\d+), \+(\d+) new", l)
            if m:
                if int(m[1]) in slots:
                    slots[int(m[1])]["piece"] = piece
                    slots[int(m[1])]["pline"] = True
                # ⛔ A pieces line ENDS its slot even when that slot's own
                # "slot N UTC" line was lost from the serial capture (it
                # happens: slot 378, 19:06:30 on 2026-11-21). Without this
                # the lost slot's decodes were added to the NEXT slot and
                # read as 20 "unconfirmed" base decodes.
                base, piece = [], []
    return [s for s in slots.values() if s.get("pline")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("wavdir")
    ap.add_argument("--boot", default=None, help="only boots whose App version line contains this")
    a = ap.parse_args()
    tot = collections.Counter()
    bad = []
    for s in parse(a.capture, a.boot):
        name = time.strftime("%y%m%d_%H%M%S", time.gmtime(s["utc"])) + ".wav"
        wav = os.path.join(a.wavdir, name)
        if not os.path.exists(wav):
            tot["no_wav"] += 1
            continue
        jp, jap, _ = V.jt9(V.REF_EXE, os.path.abspath(wav), 3, "scratchpad/jt9wd")
        # A hashed call renders as "<...>" or "<CALL>" depending on what each
        # decoder's hash table had heard - a text difference, not a different
        # message. Compared with every <...> token folded to one mark.
        ref = {hfold(t) for t in jp | jap}
        b = {hfold(V.norm(t)) for t in s["base"]}
        p = {hfold(V.norm(t)) for t in s["piece"]} - b
        tot["slots"] += 1
        tot["jt9"] += len(jp)
        tot["base"] += len(b); tot["base_ok"] += len(b & ref)
        tot["piece"] += len(p); tot["piece_ok"] += len(p & ref)
        tot["jt9_missed_by_both"] += len(jp - b - p)
        bad += [(name, t) for t in sorted(p - ref)]
    n = tot["slots"] or 1
    print(f"slots scored {tot['slots']} (no wav {tot['no_wav']}); jt9 plain {tot['jt9']} ({tot['jt9']/n:.2f}/slot)")
    print(f"  base  {tot['base']:5}  confirmed {tot['base_ok']:5}  unconfirmed {tot['base']-tot['base_ok']}")
    print(f"  piece {tot['piece']:5}  confirmed {tot['piece_ok']:5}  unconfirmed {tot['piece']-tot['piece_ok']}"
          f"   gain {100*tot['piece_ok']/max(1,tot['base']):.1f} % confirmed over base")
    print(f"  jt9 plain decodes neither found: {tot['jt9_missed_by_both']}")
    for name, t in bad[:40]:
        print(f"  UNCONFIRMED {name}  {t}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
