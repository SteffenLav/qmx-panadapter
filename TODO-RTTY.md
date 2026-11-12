# RTTY - todo list (branch `qmx-panadapter-rtty`)

All RTTY-only work lives here, not in `TODO.md` (operator, 2026-11-12).
It was T196 / 10b there. Design and measurements: `docs/rtty-feasibility.md`.

IDs are `Rnnn`, three digits, **permanent and never reused**; the next free
number goes to the next item. They are not the study's phases "R1"-"R6"
(one digit), which name stages of the June plan.

Bench: **bench2** is the RTTY bench (operator, 2026-11-12) - flash it when
needed; every flash wedges its QMX and needs his power cycle.

## Open

| ID | Item | Effort | Blocked on | Who |
|---|---|---|---|---|
| R004 | **Decode a real recording**, checked against fldigi or MMTTY on the same audio. The harness generator uses our own encoder, so only real audio is the arbiter. `test/rtty_harness.c --wav f.wav --centre Hz [--lsb] [--afc 60]`. | Minutes once a recording exists | a recording: from the operator with fldigi text, or `rxcap` on bench2 (retunes his radio) | - |
| R005 | **rx_audio tap.** Stage-1 complex baseband, 6 kHz, core 1. An open RTTY page counts as an rx_audio consumer. New `UI_MODE_RTTY` treated like CW, so the panadapter FFT keeps running (NOT the FT8/WSPR capture branch: `ui_mode_uses_iq_capture()` stops the waterfall). | Moderate | - | - |
| R006 | **RTTY page on the Tab5**, its own page modelled on the CW page (operator decision 2026-11-12): RX text, mark/space tuning aid, AFC offset, reverse. | Moderate | - | - |
| R007 | **Web RTTY panel**: RX text + tuning aid. | Moderate | R005 | - |
| R008 | **RAM and CPU on device**: demod state ~14.5 KB; core-1 load at 6 kHz. Measure on bench2. | Small | R005 + a flash | - |
| R009 | **TX timing gate**: can the QMX follow `TA<freq>;` toggles at 22 ms/bit? Dummy load, fldigi as arbiter. Host side already measured (`cat_ta_latency_probe()`). Decides whether R010 is worth writing. | Small | operator at the bench | - |
| R010 | **RTTY TX**: encoder exists (`rtty_baudot_encode`); sequencer, macros, keyboard. Must pass `tx_band_guard_allows()` and enter `docs/unattended-tx-inventory.md`. `KY` is NOT usable (no abort); `TA` stops at once. | Large | R009 | Don N2VGU asked |
| R012 | **Period bias**: the character clock settles ~0.15 % short at -6 dB. No CER cost measured; cause not found. | Investigation | - | - |
| R013 | **Coarse acquisition beyond +/-30 Hz.** AFC pulls in +/-30 Hz at -6 dB; at 40 Hz the signal's conf stays under the AFC gate and it never starts. A tone-pair search (FFT of the baseband for two peaks 170 Hz apart) on a tap or on squelch-closed would extend it. Only if a real tap-to-tune shows larger errors. | Small | R006 tap-to-tune measured | - |

## Done

| ID | Item | Commit |
|---|---|---|
| R001 | ITA2 codec + FSK demod (per-tone boxcar, character clock with flywheel) + host harness. CER 0.4 % at -6 dB SNR/2500 Hz, theory 0.3 %. Host only, never run on a device. | `a81bacb3` |
| R011 | Defaults 45.45 Bd, 170 Hz, USOS on, AFC on (+/-60 Hz). Operator: "yes, those defaults - but really you are the one to judge" (2026-11-12). Not yet wired to a setting: lands with R006. | - |
| R003 | Squelch: conf smoothed over ~5 chars, open 0.48 / close 0.44, fast close after 3 chars < 0.5, 5-char hold buffer. Noise: 0 chars in 30 min (was 79/min). -6 dB signal CER 0.3 %; ~2 junk chars after a transmission ends. AFC now also needs smoothed conf >= 0.42: noise wander 2.5 Hz rms (was 11); +/-30 Hz CER <= 4.5 %, +/-40 Hz no longer pulls in (R013). Host only. | `6a321b57` |
| R002 | AFC, gated on lock + character conf. -6 dB: +/-30 Hz CER <= 2.5 %, 0 Hz no cost, 0.2 Hz/s drift 0.9 %. Host only. | `5ceffd37` |
