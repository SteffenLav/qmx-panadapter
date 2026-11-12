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
| R005 | **rx_audio tap - BUILT, FLASHED bench2 2026-11-12, NOT YET SEEN DECODE A SIGNAL.** `main/rtty_rx.c` reads stage-1 baseband in rx_audio_task (6 kHz, core 1); RTTY is a third consumer of the chain, not an audio listener. `{"action":"rtty","on":true,"offset_hz":N}` + status/text. On the device: runs at 6000 Hz, follows CW/USB retunes, noise sig_lvl 0.36-0.38, 0 chars. Owed: one real signal, to prove the dial-offset mapping and polarity (no RTTY on 40 m, no DWD on 30 m that evening). | Small | an RTTY signal | - |
| R006 | **RTTY page on the Tab5**, its own page modelled on the CW page (operator decision 2026-11-12): RX text, mark/space tuning aid, AFC offset, reverse. | Moderate | - | - |
| R007 | **Web RTTY panel**: RX text + tuning aid. | Moderate | R005 | - |
| R008 | **RAM and CPU on device**: demod state ~14.5 KB; core-1 load at 6 kHz. Measure on bench2. | Small | R005 + a flash | - |
| R009 | **TX timing gate**: can the QMX follow `TA<freq>;` toggles at 22 ms/bit? Dummy load, fldigi as arbiter. Host side already measured (`cat_ta_latency_probe()`). Decides whether R010 is worth writing. | Small | operator at the bench | - |
| R010 | **RTTY TX**: encoder exists (`rtty_baudot_encode`); sequencer, macros, keyboard. Must pass `tx_band_guard_allows()` and enter `docs/unattended-tx-inventory.md`. `KY` is NOT usable (no abort); `TA` stops at once. | Large | R009 | Don N2VGU asked |
| R012 | **Period bias**: the character clock settles ~0.15 % short at -6 dB. No CER cost measured; cause not found. | Investigation | - | - |
| R014 | **Squelch opens on 'E'/'T' junk at the audio passband edge** (centre 2600-2700 Hz with a 2500-2700 Hz filter): only one tone has noise, so single-bit codes pass at high conf. Seen in all 5 rxcap WAVs. The firmware tap is I/Q baseband with no such edge, so it may not occur on the device - check there before fixing. | Investigation | R005 | - |
| R015 | **Contest QRM and clipping.** On VE3NEA's PA4PS (BARTG, several signals) we copy PA4PS ~5x vs GRITTY ~8x; on DL2AL (clipped) we miss "599 247-1711 TNX/73" that MMTTY and GRITTY both get. The per-tone boxcar has -13 dB sidelobes, so a strong neighbour leaks in. Candidate: a band filter around the pair before the tone filters. Measure on these two files first. | Moderate | - | - |

## Done

| ID | Item | Commit |
|---|---|---|
| R001 | ITA2 codec + FSK demod (per-tone boxcar, character clock with flywheel) + host harness. CER 0.4 % at -6 dB SNR/2500 Hz, theory 0.3 %. Host only, never run on a device. | `a81bacb3` |
| R011 | Defaults 45.45 Bd, 170 Hz, USOS on, AFC on (+/-60 Hz). Operator: "yes, those defaults - but really you are the one to judge" (2026-11-12). Not yet wired to a setting: lands with R006. | - |
| R004 | Real-signal check: VE3NEA's 5 GRITTY demo WAVs scored against MMTTY/GRITTY (`test/wav_rtty/README.md`): equal on 3, behind on the 2 contest files (R015). | `91a08926` |
| R013 | Coarse acquisition: 0.5 s DFT at 750 Hz over +/-(range + shift/2), jumps AFC to the pair when no signal is present. Synthetic -6 dB: +/-140 Hz CER <= 2.9 %; noise: 0 jumps in 20 min. Real: II1SAP (80 Hz off) went from nothing to a full copy. | `91a08926` |
| R003 | Squelch: conf smoothed over ~5 chars, open 0.48 / close 0.44, fast close after 3 chars < 0.5, 5-char hold buffer. Noise: 0 chars in 30 min (was 79/min). -6 dB signal CER 0.3 %; ~2 junk chars after a transmission ends. AFC now also needs smoothed conf >= 0.42: noise wander 2.5 Hz rms (was 11); +/-30 Hz CER <= 4.5 %, +/-40 Hz no longer pulls in (R013). Host only. | `6a321b57` |
| R002 | AFC, gated on lock + character conf. -6 dB: +/-30 Hz CER <= 2.5 %, 0 Hz no cost, 0.2 Hz/s drift 0.9 %. Host only. | `5ceffd37` |
