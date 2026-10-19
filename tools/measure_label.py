#!/usr/bin/env python3
"""How wide will this label be on the Tab5, in pixels - without a flash.

    python tools/measure_label.py "Never auto-call worked-before"
    python tools/measure_label.py --font 28 "Restart the Tab5"

WHY THIS EXISTS. A label that is too long for its column is CLIPPED, and the
code reads as though it had fitted: "Show CQs during my run" went out as
"Show CQs during my r" and only an eye on the glass caught it. The comments
in ft8_filter_modal.c therefore carry character counts as a budget - but
Montserrat is proportional, so a character count is a proxy for the thing
that actually matters and a bad one. "Skip TX1 (send report first, no grid)"
is 37 characters and 415 px; "Show CQs during my run" is 22 characters and
307 px, and it was the SHORT one that got clipped, because it sits in the
narrow right-hand column.

So measure the pixels. The numbers come from the font LVGL actually compiles
in - src/font/lv_font_montserrat_NN.c - by summing each glyph's adv_w, which
that file stores in 1/16 px. This is the same arithmetic lv_text_get_width()
does for a plain label, minus kerning (LVGL's kerning is small and NEGATIVE,
so a fit here is conservative).

WHAT IT DOES NOT TELL YOU. Only the width of the string. Where the label
STARTS, and what it runs into, are layout - read the x in the caller. For
ft8_filter_modal.c the left column starts at about x=56 (checkbox at x=4,
~44 px wide, +8 gap) and the right column begins at x=700, so the left
budget is around 640 px; the right column's own bound is somewhere between
248 px (fits) and 307 px (clipped).

⚠ It reads managed_components/, which is git-ignored and is wiped by
`idf.py fullclean` or a release's dependency refresh. Build once first if it
cannot find the font.
"""
import argparse, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def advances(size):
    path = os.path.join(ROOT, "managed_components", "lvgl__lvgl", "src",
                        "font", "lv_font_montserrat_%d.c" % size)
    if not os.path.exists(path):
        sys.exit("no font file at %s - build once so managed_components/ exists" % path)
    src = open(path, encoding="utf-8", errors="replace").read()
    try:
        blk = src[src.index("glyph_dsc[] = {"):]
    except ValueError:
        sys.exit("%s has no glyph_dsc[] - did the LVGL font format change?" % path)
    blk = blk[:blk.index("};")]
    adv = [int(m.group(1)) for m in re.finditer(r"\.adv_w\s*=\s*(\d+)", blk)]
    # cmap range_start=32, range_length=95, glyph_id_start=1: ASCII 32..126 is
    # contiguous from index 1. Anything outside that lives in the sparse second
    # cmap and is not handled here - refused below rather than guessed at.
    if len(adv) < 96:
        sys.exit("glyph table too short (%d) - unexpected font build" % len(adv))
    return adv


def width_px(text, adv):
    total = 0
    for ch in text:
        o = ord(ch)
        if not 32 <= o <= 126:
            sys.exit("non-ASCII %r: this script only covers the contiguous "
                     "ASCII cmap, which is what the UI labels use" % ch)
        total += adv[1 + (o - 32)]
    return total / 16.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text", nargs="+")
    ap.add_argument("--font", type=int, default=24,
                    help="Montserrat size the label is drawn at (default 24)")
    a = ap.parse_args()
    adv = advances(a.font)
    for t in a.text:
        print("%7.1f px  %2d ch  montserrat_%d  %r" %
              (width_px(t, adv), len(t), a.font, t))


if __name__ == "__main__":
    main()
