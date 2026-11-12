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
| R008 | **RAM and CPU on device**: demod state ~14.5 KB; core-1 load at 6 kHz. Measure on bench2; no CPU instrument exists for it yet. | Small | - | - |
| R009 | **TX timing gate**: can the QMX follow `TA<freq>;` toggles at 22 ms/bit? Dummy load, fldigi as arbiter. Host side already measured (`cat_ta_latency_probe()`). Decides whether R010 is worth writing. | Small | operator at the bench | - |
| R010 | **RTTY TX**: encoder exists (`rtty_baudot_encode`); sequencer, macros, keyboard. Must pass `tx_band_guard_allows()` and enter `docs/unattended-tx-inventory.md`. `KY` is NOT usable (no abort); `TA` stops at once. | Large | R009 | Don N2VGU asked |
| R012 | **Period bias**: the character clock settles ~0.15 % short at -6 dB. No CER cost measured; cause not found. | Investigation | - | - |
| R014 | **Squelch opens on 'E'/'T' junk at the audio passband edge** (centre 2600-2700 Hz with a 2500-2700 Hz filter): only one tone has noise, so single-bit codes pass at high conf. Seen in all 5 rxcap WAVs. The firmware tap is I/Q baseband with no such edge, so it may not occur on the device - check there before fixing. **Seen on the device 2026-11-12**: a narrow non-RTTY signal (~50 Hz, 7.04813 MHz) under ONE line - M full, S empty - opened the squelch (sig 0.62) and printed `IXFYUVLH ODKXZV`. One observation. Squelch on noise alone, by contrast: 0 chars in 600 s on an empty offset (bench2, 40 m faded, sig max 0.42 < open 0.48). Candidate fix: hold the squelch shut while one tone is near the floor (`mark_snr_db`/`space_snr_db` exist since `579b63fe`). | Small | - | - |
| R019 | **RTTY settings do not survive a reboot**: offset, reverse, baud/shift, AFC, squelch return to defaults (no NVS or config-file use in `rtty_rx.c`); R011's defaults were never wired to a setting. Seen 2026-11-12: -1960 Hz came back as +1500 after a flash. | Small | - | - |

## Done

| ID | Item | Commit |
|---|---|---|
| R001 | ITA2 codec + FSK demod (per-tone boxcar, character clock with flywheel) + host harness. CER 0.4 % at -6 dB SNR/2500 Hz, theory 0.3 %. Host only, never run on a device. | `a81bacb3` |
| R011 | Defaults 45.45 Bd, 170 Hz, USOS on, AFC on (+/-60 Hz). Operator: "yes, those defaults - but really you are the one to judge" (2026-11-12). Not yet wired to a setting: lands with R006. | - |
| R015 | Band prefilter (centre to 0 Hz, 8-pole Butterworth at shift/2 + 0.6 baud) before the tone filters. Synthetic, wanted at 0 dB: a +20 dB station 250 Hz off 88 % -> 7 % CER, 300 Hz 54 % -> 0 %. No cost at -6/-8 dB (12000 chars). VE3NEA set 44 -> 61 exchange words; DL2AL 3 -> 15. PA4PS still behind GRITTY (3 -> 4) - not explained. | `fac3516e` |
| R007 | Web RTTY panel: menu link, MODE: RTTY, 8 rows over the waterfall (measured width), controls row, tone markers, click moves the decoder; same page rules as the Tab5 (pills, DiGi band plan, RTTY spots, no CW line). Previewed via an /api proxy; markers need the WS and are unseen. | `e59a2432` |
| R018 | Text box clear of the watermark (60 cols), carpet shift like CW. /ss.bmp: border x 1242, watermark clear. Frame time not measured. | `31ac6638` |
| R005 | Device tap decodes a real signal: "CQ CQ DE ON8FAB" on bench2, 40 m, radio in CW, 2026-11-12. First try failed: in CW the decoder sat one pitch (550 Hz) below the lines; fixed. | `1c0cf073` |
| R017 | Top bar Mode/BW dimmed; band-plan strip DiGi-only (drag maps onto it, tuning not clamped); spots RTTY-only (tag RY); QMX-wait hint at the CW page's place. Operator on the glass: "looks right". | `4a811279` |
| R016 | Pane fonts 18/20/22 -> 24/26 (picker 28), buttons word-only with green = on. Operator: "it actually looks good"; decoded text stays qmx_mono_25, 8 rows (his choice over 30 px / 6 rows). | `f0f760e5` |
| R006 | RTTY page: own page on the CW grid, 8 text rows, tuning bars, tone markers, tap moves the decoder (dial stays), speed/shift picker, MODE: RTTY in the left drawer (7 doors at 88 px), set_screen "rtty", restored at boot. Operator on the glass: "looks right for now" (fonts: R016). Not yet seen with a real signal. | `20cfea0b` |
| R004 | Real-signal check: VE3NEA's 5 GRITTY demo WAVs scored against MMTTY/GRITTY (`test/wav_rtty/README.md`): equal on 3, behind on the 2 contest files (R015). | `91a08926` |
| R013 | Coarse acquisition: 0.5 s DFT at 750 Hz over +/-(range + shift/2), jumps AFC to the pair when no signal is present. Synthetic -6 dB: +/-140 Hz CER <= 2.9 %; noise: 0 jumps in 20 min. Real: II1SAP (80 Hz off) went from nothing to a full copy. | `91a08926` |
| R003 | Squelch: conf smoothed over ~5 chars, open 0.48 / close 0.44, fast close after 3 chars < 0.5, 5-char hold buffer. Noise: 0 chars in 30 min (was 79/min). -6 dB signal CER 0.3 %; ~2 junk chars after a transmission ends. AFC now also needs smoothed conf >= 0.42: noise wander 2.5 Hz rms (was 11); +/-30 Hz CER <= 4.5 %, +/-40 Hz no longer pulls in (R013). Host only. | `6a321b57` |
| R002 | AFC, gated on lock + character conf. -6 dB: +/-30 Hz CER <= 2.5 %, 0 Hz no cost, 0.2 Hz/s drift 0.9 %. Host only. | `5ceffd37` |
