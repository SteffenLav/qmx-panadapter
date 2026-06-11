# QMX vendor reference manuals

This folder is a local cache for two QRP Labs vendor PDFs used as authoritative
references while building FT8 features (decode calibration, and the v0.12.0
Manual FT8 TX work in particular):

- `QMX_CAT_programming_manual_1.03.000.pdf` — QMX CAT Programming Manual, firmware 1.03_000
- `QMX_operation_manual_1.03.002.pdf` — QMX Operating Manual, firmware 1_03_002

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
