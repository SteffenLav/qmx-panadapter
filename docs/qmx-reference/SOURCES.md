# QMX vendor reference manuals

This folder is a local cache for QRP Labs vendor PDFs used as authoritative
references while building FT8 features (decode calibration, and the v0.12.0
Manual FT8 TX work in particular):

- `QMX_CAT_programming_manual_1.03.000.pdf` — QMX CAT Programming Manual, firmware 1.03_000
- `QMX_operation_manual_1.03.002.pdf` — QMX Operating Manual, firmware 1_03_002
- `QMX_CAT_manual_1.04.001.pdf` — QMX CAT Programming Manual, firmware 1_04_001 (covers all
  1_04 CAT changes; 1_04_002 added no CAT changes). Added 2026-07-03 for the 1_03↔1_04
  comparison in `docs/qmx-1_04-cat-comparison.md`.
- `QMX_operation_manual_1.04.001.pdf` — QMX Operating Manual, firmware 1_04_001 (same purpose)
- `QMX_CAT_manual_1.04.004.pdf` — QMX CAT Programming Manual, **firmware 1_04_004 and above**
  (document revision 1_04_004, 23-Jul-2026). Added 2026-08-29. **This is the current one** —
  it covers 1_04_004 through 1_04_008 and documents six commands the 1_04_001 manual does
  not: `BD`, `BN`, `BU`, `UI` (added 1_04_003) and `GP`, `SR` (added 1_04_004).
  ⚠ Its page footers still say "firmware 1_04_003"; the document revision history is the
  authority. Extract: `cat_104_004.txt`.

- `QDX_operation_manual_1_10.pdf` — **QDX** Operating Manual, firmware 1_10 (19-Jul-2023;
  includes its full CAT command list — QDX has no separate CAT manual). Added 2026-08-29 to
  answer whether the panadapter could support a QDX (Travis AK6TB). Extract: `qdx_op_110.txt`.
  **This IS the current QDX manual** — verified 2026-08-31: QDX firmware downloads stop at
  `1_10_.zip`, so the firmware has been frozen since Jul-2023. (`manual_1_12.pdf` /
  `manual_1_24.pdf` on the QDX page are the kit ASSEMBLY manuals, not firmware 1_12.)

  Full analysis: **`docs/qdx-vs-qmx-comparison.md`**. Headlines, so nobody re-derives them:
  QDX has the **same `Q9` IQ mode** (same session-only caveat) and the **same 48 ksps 24-bit
  stereo I/Q**, and no `TA`, `PC`, `SW`, `MM`, `TM` or `RG`. `MD` is USB/LSB only. `VN;`
  returns `VN1_10;` with no "QMX" suffix — a reliable way to tell the radios apart. `AG` is
  in **plain dB** on a QDX, not the QMX's 0.25 dB steps, so our `AG0nnn;` asks for 4x too much.

  ⚠ **Two corrections to the note that stood here from 2026-08-29 to 2026-08-31:**
  1. It said QDX has "the **same 12 kHz IF** — so the receive DSP would carry across".
     **Probably wrong.** The firmware 1_06 changelog says *"12kHz IF offset is removed when
     you enable IQ Mode"*, i.e. a QDX likely delivers TRUE BASEBAND and our `n_bins/4` shift
     would put every signal 12 kHz off. Changelog-sourced, not in the manual body — confirm on
     hardware before building on either reading.
  2. It implied the missing `TA` made transmit impossible. **Also wrong.** The Tab5 is the USB
     HOST, so it can stream synthesised PCM into the QDX's sound-card output exactly as a PC
     does — `uac_host_device_write()` exists in our patched UAC component, `audio.c` already
     receives (and ignores) `UAC_HOST_DRIVER_EVENT_TX_CONNECTED`, and `synth_gfsk_heap()`
     already generates FT8 audio on-device. TX is a separate engine, not a wall. The real
     constraint is that the QDX **disables transmit while IQ mode is on** (same changelog), so
     `Q9` must be toggled around every burst — and that toggle also moves the LO by 12 kHz.

**Not yet cached** (newer than the above, noted 2026-08-29): operating manual for
1_04_004 and above (23-Jul-2026), and the Virtual U3S manual for **1_04_008a** and above
(28-Aug-2026) — the "008a" implies a firmware revision past 1_04_008.

`cat_103.txt` / `cat_104.txt` / `cat_104_004.txt` are local `pdftotext -layout` extracts of the CAT
manuals (also gitignored) — regenerate with `pdftotext -layout <pdf> <txt>` if missing.

**These PDFs are not committed to the repo** (see `.gitignore`). They are
copyrighted vendor documentation (© QRP Labs / Hans Summers G0UPL), and this is
a public MIT-licensed repo — redistributing them here would be a copyright grey
area at best. Keeping them local-only avoids that, while still letting anyone
working on this codebase (including future Claude sessions — the files persist
on disk and are readable even though git ignores them) reference the same
material.

Download your own copy from the QMX product page:

- http://qrp-labs.com/qmx (links to current manuals and firmware)

## Why these matter for FT8 TX (v0.12.0)

The CAT manual documents the `TA` (Transmit Audio) command, which is the key
that unlocks a *much* simpler TX design than initially assumed:

```
FA<freq>;     set USB dial frequency
TX;           switch to transmit (key-down, Blackmann-Harris envelope)
TA<freq.f>;   set transmitted audio tone frequency (decimal Hz precision),
              repeat for each FT8 symbol at the ~160 ms cadence
TA0;          key-up (any value < 10 Hz), Blackmann-Harris envelope
              ...wait ~5 ms for envelope shaping to finish...
RX;           back to receive
```

The radio does its own DDS synthesis (Si5351/MS5351M) and envelope shaping —
**no PCM waveform synthesis or USB audio playback is needed**. This is actually
*more* precise than the conventional WSJT-X workflow described in the operation
manual (§6), where the PC streams real audio over the USB sound card and QMX
*measures* the tone frequency via zero-crossing detection (subject to
quantisation noise, "Rise/Fall threshold" and "Minimum samples" tuning, etc).
Driving `TA` directly sidesteps all of that measurement chain entirely.

PTT is equally simple: `TX;` / `RX;` (equivalent to `TQ1;` / `TQ0;`).
