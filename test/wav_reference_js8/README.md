# JS8 reference corpus

Empty on purpose. There are 35 FT8 references in `../wav_reference` and this
directory is the JS8 equivalent (M1 in `docs/js8-feasibility.md`). It is filled
from a running board:

    python tools/js8_corpus.py 192.168.1.210 --out test/wav_reference_js8

Each slot arrives as `YYMMDD_HHMMSS.wav` (12 kHz, mono, 16-bit, 15.0 s -
exactly the shape of the FT8 references) plus a `.txt` of what the board
decoded for it. Check them with:

    ./js8_wav_harness test/wav_reference_js8

## Two things the files are not

1. **Not ground truth.** The `.txt` is this firmware's own decode list, so the
   harness is a REGRESSION floor: it fails when a change decodes less than
   today's build. A slot JS8Call would have decoded and we did not is recorded
   as empty, and the harness will call that slot a pass.

2. **Not the live signal level.** The stored audio is normalised to full scale.
   `monitor_process()` packs each bin as a clamped `2*db + 240` over -120..0 dB,
   so an un-normalised slot can saturate the waterfall flat and decode nothing.
   `/api/slot.json` reports `peak_float` - the decoder's own float peak before
   normalisation - and that is the number to look at if a corpus slot decodes
   and the board did not.
