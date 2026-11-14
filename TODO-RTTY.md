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
| R009 | **TX timing gate**: can the QMX follow `TA<freq>;` toggles at 22 ms/bit? Dummy load, fldigi as arbiter. Host side already measured (`cat_ta_latency_probe()`). Decides whether R010 is worth writing. Burst BUILT + flashed bench2 (`2d15d857`): `{"action":"rtty_tx_test","centre_hz":1500,"seconds":20}`, DiGi, dial 14.080.00, dummy load; PC receiver + fldigi. Not yet keyed. | Small | operator at the bench | - |
| R022 | Boot after a flash: the Tab5 sent `FA 14085000` at 29.6 s, 125 ms after the band list, no `RTTY entry` line (bench2 2026-11-12 22:43, draft of `dde73aff`). 14.085 = the DiGi-middle fallback, NOT the stored 20 m dial (14.081 - every store logs). The 2nd boot hit (31.4 s, `55b1ae24`) was a band press. Silent writers are all operator gestures; log cannot tell them apart, so every `Sent: FA` now names its caller (addr2line it). Wait for a recurrence. | Investigation | recurrence | - |
| R010 | **RTTY TX**: encoder exists (`rtty_baudot_encode`); sequencer, macros, keyboard. Must pass `tx_band_guard_allows()` and enter `docs/unattended-tx-inventory.md`. `KY` is NOT usable (no abort); `TA` stops at once. | Large | R009 | Don N2VGU asked |

## Done

| ID | Item | Commit |
|---|---|---|
| R025 | RTTY tap on a 10 Hz grid, Tab5 and web (was the 500 Hz DiGi dial grid: up to 250 Hz off the pair, outside acquisition's +/-150 Hz). bench2 taps at +830/-2670/+740/+2890; operator: "Locks on". | `4d42824d` |
| R024 | RTTY page: the Tab5 speaker plays the signal under the lines (NCO1 on the decoder's frequency, tone centre 2210 Hz = 2125/2295, 1 kHz filter, mode forced USB). It played the radio mode's passband. Operator on bench2: "Hear it now". QMX headphones still play the radio's passband. | `ad19cd07` |
| R023 | Decoder listened 6 kHz off the lines: it read the audio path's stage 1 (6 kHz wide around dial +1800 in DiGi), so a tap further out aliased. bench2: lines at -2959, decoder at +3041. Now its own NCO + /8 decimator on the 48 kHz I/Q; `baseband centre +0 Hz` on every tap. Copied RD0A at +3176 and IV3SKB at +400 (inside the old window), and YT2RX at -1690 (OUTSIDE it - old build listened at +4310): proven. | `ad19cd07` |
| R021 | RTTY page band change: dial to the stored RTTY dial for that band (NVS blob `bandrtty`), else mid DiGi segment (whole kHz). Stored on leaving a band on the page and on leaving the page. bench2: 8 bands landed mid DiGi (80 m 3.590, 40 m 7.059, 20 m 14.085 ...); 20 m recalled 14.083/14.081 after band round trips, page exit and a reboot. Tab5 band popup uses the same function, not exercised by hand. Entering the page now jumps to the band's RTTY dial (operator: "Yes, jump on entry"); bench2: pan 40 m 7.150 -> RTTY entry 7.047 (stored). | `49e99225` `55b1ae24` `785b8f6d` `dde73aff` |
| R020 | Left drawer: MODE: RTTY under MODE: CW, 7 doors at 88 px in one column. Page no longer covers the left-edge strip (bench2 booted into RTTY and could not leave). bench2 flashed; grip seen over the pane on /ss.bmp, operator used the drawer RTTY->CW->RTTY; set_screen pan/cw/ft8/ft4/js8/wspr/rtty all switched, WSPR held buffers and armed a cycle. Operator on the glass: "the drawer looks right". | `04a0b1c5` |
| R012 | ❌ RETRACTED - no period bias. Mean locked period at -6 dB within +/-0.001 % (4 seeds x ~6700 slots), -8 dB +/-0.006 %; sender +/-0.3 % tracked. Slot-to-slot sd is 0.12 %: the "0.15 % short" was one reading of that jitter. Same on the R001 code. Now a harness check (fails on a 0.1 % bias, mutation-tested). | `0efbdcdf` |
| R008 | RAM/CPU on bench2. RAM: demod 18552 B PSRAM, ~3.0 KB internal static. CPU: feed timer 4.0-6.1 % of core 1 (921 s); A/B same boot, 5 min each: idle1 39.1 % on / 42.7 % off / 39.0 % on = 3.6 points. Worst call ~13 ms = the 0.5 s acquisition DFT (host: 53 % of cost). rx_audio LATE not tied to the decoder: 14 / 7 / 0-for-200-s then ~14 per window after one 172 ms frame stall at uptime ~1520 s (POTA fetch + diag rotate, decoder untouched) - unexplained, not RTTY. | `2d15d857` |
| R001 | ITA2 codec + FSK demod (per-tone boxcar, character clock with flywheel) + host harness. CER 0.4 % at -6 dB SNR/2500 Hz, theory 0.3 %. Host only, never run on a device. | `a81bacb3` |
| R011 | Defaults 45.45 Bd, 170 Hz, USOS on, AFC on (+/-60 Hz). Operator: "yes, those defaults - but really you are the one to judge" (2026-11-12). Not yet wired to a setting: lands with R006. | - |
| R015 | Band prefilter (centre to 0 Hz, 8-pole Butterworth at shift/2 + 0.6 baud) before the tone filters. Synthetic, wanted at 0 dB: a +20 dB station 250 Hz off 88 % -> 7 % CER, 300 Hz 54 % -> 0 %. No cost at -6/-8 dB (12000 chars). VE3NEA set 44 -> 61 exchange words; DL2AL 3 -> 15. PA4PS still behind GRITTY (3 -> 4) - not explained. | `fac3516e` |
| R007 | Web RTTY panel: menu link, MODE: RTTY, 8 rows over the waterfall (measured width), controls row, tone markers, click moves the decoder; same page rules as the Tab5 (pills, DiGi band plan, RTTY spots, no CW line). Previewed via an /api proxy; markers need the WS and are unseen. | `e59a2432` |
| R018 | Text box clear of the watermark (60 cols), carpet shift like CW. /ss.bmp: border x 1242, watermark clear. Frame time not measured. | `31ac6638` |
| R005 | Device tap decodes a real signal: "CQ CQ DE ON8FAB" on bench2, 40 m, radio in CW, 2026-11-12. First try failed: in CW the decoder sat one pitch (550 Hz) below the lines; fixed. | `1c0cf073` |
| R019 | RTTY settings survive a reboot: offset, reverse, baud, shift, AFC, squelch in one NVS blob (`rtty_cfg`, DIRTY bit 160, DIRTY_WORDS 5 -> 6) + `rtty_*` lines in the config file. bench2: -1234/50/450/reverse/AFC off/squelch off came back after a reboot, and /api/config exported them. Config IMPORT not exercised. Restored to defaults after the test. | `0cfbcb9a` |
| R014 | Squelch opens only when BOTH tones stand >= 6 dB over the floor (acquisition DFT now always runs, window >= +/-150 Hz). Harness: keyed carrier on one tone 241 -> 0 chars/min; no CER cost at -6/-8/-10 dB; GRITTY set 61 = 61 words. Closing on a lost tone was tried and dropped (DL2AL 15 -> 13). Flashed bench2; not yet seen on a one-tone signal on the device. | `9c656a80` |
| R017 | Top bar Mode/BW dimmed; band-plan strip DiGi-only (drag maps onto it, tuning not clamped); spots RTTY-only (tag RY); QMX-wait hint at the CW page's place. Operator on the glass: "looks right". | `4a811279` |
| R016 | Pane fonts 18/20/22 -> 24/26 (picker 28), buttons word-only with green = on. Operator: "it actually looks good"; decoded text stays qmx_mono_25, 8 rows (his choice over 30 px / 6 rows). | `f0f760e5` |
| R006 | RTTY page: own page on the CW grid, 8 text rows, tuning bars, tone markers, tap moves the decoder (dial stays), speed/shift picker, MODE: RTTY in the left drawer (7 doors at 88 px), set_screen "rtty", restored at boot. Operator on the glass: "looks right for now" (fonts: R016). Not yet seen with a real signal. | `20cfea0b` |
| R004 | Real-signal check: VE3NEA's 5 GRITTY demo WAVs scored against MMTTY/GRITTY (`test/wav_rtty/README.md`): equal on 3, behind on the 2 contest files (R015). | `91a08926` |
| R013 | Coarse acquisition: 0.5 s DFT at 750 Hz over +/-(range + shift/2), jumps AFC to the pair when no signal is present. Synthetic -6 dB: +/-140 Hz CER <= 2.9 %; noise: 0 jumps in 20 min. Real: II1SAP (80 Hz off) went from nothing to a full copy. | `91a08926` |
| R003 | Squelch: conf smoothed over ~5 chars, open 0.48 / close 0.44, fast close after 3 chars < 0.5, 5-char hold buffer. Noise: 0 chars in 30 min (was 79/min). -6 dB signal CER 0.3 %; ~2 junk chars after a transmission ends. AFC now also needs smoothed conf >= 0.42: noise wander 2.5 Hz rms (was 11); +/-30 Hz CER <= 4.5 %, +/-40 Hz no longer pulls in (R013). Host only. | `6a321b57` |
| R002 | AFC, gated on lock + character conf. -6 dB: +/-30 Hz CER <= 2.5 %, 0 Hz no cost, 0.2 Hz/s drift 0.9 %. Host only. | `5ceffd37` |
