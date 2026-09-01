# QDX vs QMX — what it would take to support a QDX

Written 2026-08-31 in answer to the recurring user request "can I use my QDX
instead of a QMX?". Everything below is from the vendor manuals and the QRP Labs
firmware changelog, cross-checked against our own code. **Nothing here has been
tested against a real QDX — there is no QDX on the bench** (see
`docs/bench-setup.md`: dev = QMX, lab = QMX+, field and port have no radio).

## Why this is worth more on a QDX than it is on a QMX

The QDX has **no user interface at all**. The manual: *"Only three connectors:
USB (audio and serial for CAT), Power and RF"*, plus a single 3 mm red status
LED. No display, no encoder, no buttons. It was designed to be a headless box on
the end of a PC's USB cable and it cannot be operated any other way.

A QMX already has an LCD, an encoder and buttons — the Tab5 makes it nicer. A
QDX has nothing, so the Tab5 would be the only face it has ever had. That turns
a PC accessory into a self-contained portable radio, which is a bigger jump than
anything this project does for a QMX.

That argument only pays off if transmit works, because a receive-only QDX still
needs the PC for the half that matters. Which is why the TX section below is the
whole question.

(QDX-M, the single-band version, runs the same firmware and is covered by
everything here.)

## Bottom line

- **Receive: straightforward.** Same 48 ksps 24-bit stereo I/Q, same `Q9` IQ
  mode, same Kenwood CAT basics. Perhaps a day of work, one real unknown (the IF
  offset, below).
- **Transmit: possible, but it is a second TX engine, not an adaptation of the
  one we have.** The QDX has no `TA`, so tones cannot be commanded. They have to
  be **synthesised as PCM on the Tab5 and streamed into the QDX's USB sound
  card**, which is exactly what WSJT-X does. We are the USB host, the driver
  already exposes `uac_host_device_write()`, and we already synthesise FT8 audio
  on-device. It is real work with real risk, not a wall.

⚠ **This document originally said transmit was impossible. That was wrong** — it
reasoned from the absence of `TA` and stopped there, without checking whether the
Tab5 could drive the QDX's audio input the way a PC does. It can. The corrected
analysis is below.

## Sources

| Document | Version | Date | Notes |
|---|---|---|---|
| `QDX_operation_manual_1_10.pdf` | firmware 1_10 | 19-Jul-2023 | **Current.** QDX has no separate CAT manual — the command set is section 4.10 of the operating manual |
| `QMX_CAT_manual_1.04.004.pdf` | firmware 1_04_004+ | 23-Jul-2026 | Current QMX CAT manual |
| qrp-labs.com/qdx changelog | — | — | Source of two IQ-mode facts the manual omits entirely |

Both PDFs are in `docs/qmx-reference/` (gitignored — vendor copyright), with
`pdftotext -layout` extracts alongside.

⚠ **QDX firmware is frozen.** The last release is `1_10_.zip`, July 2023 — three
years old. Hans's effort moved to the QMX. Do not plan around a QDX firmware
change; in particular, do not wait for `TA`.

⚠ **The QDX manual's command list is not exhaustive.** The changelog records a
`TP` command (STM32 temperature sensor) that never appears in the manual's
alphabetical listing. Treat the list as near-complete, not authoritative.

⚠ `manual_1_12.pdf` / `manual_1_24.pdf` on the QDX page are the **kit assembly**
manuals, not firmware 1_12 operating manuals. Easy to grab by mistake — I did.

## CAT command diff

Commands **we send today** that the QDX does not have:

| Command | What we use it for | Consequence on a QDX |
|---|---|---|
| `TA` | every FT8/FT4/WSPR transmission | tones must be synthesised as PCM instead — see below |
| `MM` | SSB filter width, CW passband/centre, PA voltage guard, GPS source, band config | returns `?;`. Kills the SSB filter dance, the WSPR PA guard, GPS detection |
| `PC` / `SW` | power + SWR readout, antenna tune, WSPR measured power | no tune screen, no measured power, no PA protection |
| `TM` | QMX clock read/push — the offline/POTA time source | no time fallback without WiFi |
| `RG` | RF gain slider | not over CAT (QDX has per-band RF gain in its terminal config only) |
| `RC` | clear RIT before every `RU`/`RD` write | RIT needs its own write strategy |
| `LC` | read the radio's LCD | n/a — there is no LCD |
| `KY` `KS` `KD` | CW keying | QDX has no CW |
| `SM` `SA` `SS` `PS` `TR` `RR` `BD` `BN` `BU` `UI` `GP` `SR` `OM` `AI` | assorted QMX-only | unused or degraded |

Present on **both**, with identical semantics:
`FA` `FB` `FR` `FT` `FW` `ID` `IF` `MD` `RD` `RU` `RT` `RX` `SP` `TQ` `TX` `VN`
`AG` `C2`, and the whole `Q0`–`QB` extended set — **including `Q9` (IQ mode)** and
`Q3` (VOX), with the same session-only, not-in-EEPROM caveat we already handle.

QDX-only: `QD`–`QJ` (extra serial ports, VGA/PS2 terminal, night mode, TX shift
threshold). Nothing we want.

## Transmit: what it would actually take

**The mechanism.** A PC transmits on a QDX by playing audio into the QDX's USB
sound-card output while holding PTT over CAT. The QDX then *measures* the tone by
zero-crossing detection and synthesises a clean single signal at that frequency.
The Tab5 is the USB host on that same cable, so it can do the same thing:

1. `TX;` (CAT PTT — the QDX manual explicitly recommends CAT over VOX).
2. Stream synthesised PCM into the QDX's UAC output endpoint for the burst.
3. `RX;`.

**Three pieces of it we already have.** `uac_host_device_write()` is in the
patched UAC component we ship. `audio.c`'s driver callback already receives
`UAC_HOST_DRIVER_EVENT_TX_CONNECTED` — it currently logs it and ignores it. And
`synth_gfsk_heap()` in `ft8_test.c` already generates FT8 GFSK audio on-device
(it is what sim mode and the Field Day end-to-end self-test run through), so the
waveform generator is written and validated.

**What is genuinely new work:**

- **A real-time PCM feeder.** ~12.6 s of 48 ksps audio per FT8 burst has to be
  generated and written continuously without underrunning. The synthesis is
  cheap, but it is new hard-real-time work on a board where **core 0 sits at
  0–7 % idle** and taskLVGL owns 73.9 % of it. This belongs on core 1, below
  `fft_task` — and CLAUDE.md already records what happens when that rule is
  broken.
- **The output alt-setting.** We read format from the descriptor for RX and would
  do the same for TX, but the QDX's speaker endpoint format is unmeasured. It
  will not necessarily match the 24-bit stereo input side.
- **Toggling `Q9` around every burst — the real constraint.** Firmware 1_06
  changelog: *"Transmit is disabled when you are in IQ mode."* So the panadapter
  must be turned **off** to transmit and back on afterwards. Losing the display
  during TX is fine (it is useless then anyway), but the changelog also says
  enabling IQ mode **removes the 12 kHz IF offset**, which means toggling `Q9`
  retunes the LO by 12 kHz each way. Whether that settles inside FT8's slot
  timing is unknown and is the single biggest schedule risk in this whole
  document. Our dead-stream watchdog and flat-floor reset would both need to be
  told this is deliberate.
- **Tone accuracy is measured, not commanded.** `TA` sidesteps the entire
  measurement chain — SOURCES.md already notes it is *more* precise than the
  WSJT-X workflow. On a QDX we inherit the zero-crossing path and its
  Rise/Fall-threshold and Cycle/Sample-Min tuning. Thousands of WSJT-X users
  prove it works; it is still a quality risk we do not have today, and WSPR
  (very narrow tone spacing) is where it would bite first.
- **Trivia by comparison:** the `DiGi` mode pre-flight in `ft8_tx.c` requires
  mode digit 6 and would abort every burst on a USB-only QDX. That is a
  per-radio relaxation, not a problem.

**No PA protection.** No `PC`, no `SW`, no `MM` — so no measured power, no SWR,
no antenna tune, and no equivalent of the WSPR PA-voltage guard. On a mode that
keys for 110 s out of every 120, that is worth thinking about before enabling
WSPR TX on a QDX at all.

## Receive: what has to change

**1. The 12 kHz IF offset is probably absent — and it is the thing most likely to
be silently wrong.** The QMX places its LO 12 kHz off the dial, so the tuned
signal lands at +12 kHz in the baseband I/Q, and `dsp.c` / `ui.c` shift bin
selection by `n_bins/4` to re-centre it. The QDX changelog says *"12kHz IF offset
is removed when you enable IQ Mode"*, and the manual corroborates it indirectly —
IQ mode feeds the ADC channels to USB *"directly, without any demodulation"*,
bypassing the DSP superhet. If so, a QDX delivers true baseband and our
`n_bins/4` shift puts every signal 12 kHz off, with tap-to-tune wrong to match.

Cheap to handle (make the shift a per-radio value, 0 or `n_bins/4`) but **it must
be confirmed on hardware**: it rests on one changelog line, not the manual body.

**2. Radio identification.** `VN;` returns `VN1_10;` (or `VN1_05_003;` on some
builds) — **no `QMX` suffix**, which is a clean discriminator.
`cat_qmx_fw_at_least()` parses `%d_%d_%d` and returns false on a 2-part string,
so existing version gates already fail safe. Add a `radio_type_t` resolved from
`VN;` plus a capability struct, rather than sprinkling `if (is_qdx)`.

**3. `AG` is scaled differently — we would set the volume 4× too high.** QMX `AG`
is in **0.25 dB steps**, 3-digit: `AG0091;` = 22.75 dB. QDX `AG` is in **plain
dB**: `AG21;` = 21 dB. Our `AG0%03u;` asks a QDX for 91 dB.

**4. `FW;` is get-only and always returns 3200.** No selectable SSB filter, so
the whole `MMSSB|Filter RX=` / `Bandwidth=` / FW-suppression dance is inert on a
QDX — harmlessly, since nothing to pin means nothing to revert. The passband
overlay needs a fixed QDX width.

**5. RIT without `RC`.** We send `RC;` before every `RU`/`RD` because the QMX
interprets those as absolute *or* relative depending on a menu setting. The QDX
documents them as unambiguously absolute and has no `RC` — so it needs its own
path, clearing with `RU0;`.

**6. One serial port, not two.** The QMX exposes a second CDC interface
(interface 5) which is what the "Radio menus" screen uses. The QDX has a single
virtual COM port, and its terminal mode takes that port over and **disables CAT
while active**. That screen has to be hidden on a QDX; it cannot work without
dropping the panadapter.

**7. Things that carry across untouched.** The audio path — QDX is 48 ksps 24-bit
stereo I/Q like the QMX, and `audio.c` already reads channels/bit-depth/rate from
the alt-setting descriptor, with 6-byte-frame unpacking that matches byte for
byte; I expect **zero changes** there. Also the `Q9` handshake, `Q3 0;` VOX-off,
`FA` tuning, band changes, the FFT, waterfall, decoders, ADIF, PSK Reporter,
uploads and the web UI. And `dsp/iq_balance.c` becomes *more* valuable: the QDX
manual says outright that *"no attempt is made to compensate"* for I/Q amplitude
and phase error in the radio.

## Effort

| Phase | Work | Estimate |
|---|---|---|
| 1 | `radio_type_t` + capability struct from `VN;`, threaded through `cat.c` | half a day |
| 2 | Per-radio IF shift, `AG` scaling, fixed `FW`, RIT without `RC` | half a day |
| 3 | UI gating for absent features (tune, CW, RF gain, radio menus, PA guard) | half a day |
| 4 | **TX: PCM synthesis + real-time UAC write feeder + `Q9` toggling** | several days, plus bench time |

So roughly **1.5–2 days for a receive-only QDX**, and **TX is a separate project**
on top — one that needs measurement before it can even be estimated honestly
(`Q9` settling time, the output endpoint format, tone accuracy under the
zero-crossing chain).

**The blocker is the radio, not the code.** None of this can be verified without
a QDX on the bench, and the two decisive facts — the missing IF offset and
TX-disabled-in-IQ-mode — come from a changelog, not the manual. Options: buy a
QDX (they are inexpensive), borrow one, or ship receive-only behind an explicit
"QDX (untested)" flag with a willing user. The ST7121 display variant was done
that way and it cost several wrong releases precisely because nobody had the
hardware.

## Suggested order

1. **Get a QDX on the bench.** Everything else is speculation until then, and the
   two riskiest facts are both cheap to settle in an hour with one attached.
2. **Ship receive-only first.** It is ~2 days, it is low-risk, and it is
   immediately useful — a QDX owner gets a spectrum, waterfall, tap-to-tune and
   FT8/FT4/WSPR decode lists on a radio that previously showed them a single red
   LED.
3. **Then decide on TX** with the `Q9` toggle measured rather than guessed. If
   the LO settles fast enough, a QDX plus a Tab5 becomes a genuinely standalone
   FT8 station, which is the outcome that makes this worth doing at all.
