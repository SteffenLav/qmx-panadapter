# JS8 reference corpus

10 slots recorded off bench dev on 2026-10-09 - 4 with decodes, 6 without.
This is the JS8 equivalent of the 35 FT8 references in `../wav_reference`
(M1 in `docs/js8-feasibility.md`). Check them with the harness, which also
runs as part of `tools/run_harnesses.py`:

    ./js8_wav_harness                        # selftest + this directory

What is in it:

| Slot | Dial | Decode |
|---|---|---|
| `261009_112345` | 14.078 | `HB F4LPU JN15` at -12 |
| `261009_113345` | 7.078 | `G4GHL LA7HKA HEARTBEAT SNR` at -3 |
| `261009_113700` | 7.078 | `M0IQF LA7HKA HEARTBEAT SNR` at -16 |
| `261009_113800` | 7.078 | `9A3SWO LA7HKA HEARTBEAT SNR` at -18 |

The other six decode nothing and are kept deliberately: they are the only
thing that catches the opposite regression, a change that INVENTS decodes.
They are the six highest-`peak_float` silent slots of the 60 recorded, so they
are the ones most likely to tempt a decoder into a false positive.

⛔ **Start the recorder before touching the radio.** `slotcap` holds one slot
and each overwrites the last. Three 40 m slots that decoded 4, 2 and 1 were
lost on 2026-10-09 by retuning first and starting the recorder afterwards.

Add more:

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
