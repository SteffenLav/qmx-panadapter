# WSPR Phase 2 — TX engine status

Companion to `wspr-phase1-status.md` (RX codec + decoder). This file covers
`main/wspr_tx.c` / `.h`: building a WSPR transmission and driving it out as a
timed CAT tone sequence.

**This engine has now transmitted on the air and been decoded worldwide** — see
"ON THE AIR" below, the last section. Everything before that section was written
from **dry runs**, where `tx_cmd()` logs the command it would have sent and sends
nothing, and it is kept in that voice deliberately: the dry-run sections record
what could and could not be concluded *before* there was external evidence, which
is the more useful thing to be able to re-read.

`WSPR_TX_SEND_LIVE` still defaults to 0 in git and always has. Going on air is a
deliberate, supervised act, not a build-time default.

## First hardware run — 2026-08-23

Until this date the engine had compiled into real firmware and booted, but the
162-symbol burst had **never been observed running**. It has now run twice, end
to end, on the dev Tab5 (COM3), triggered by the dev action
`POST /api/cmd {"action":"wspr_tx_test"}`.

Structurally correct on the first attempt:

- **162 `TA<freq>;` commands**, one `TX;`, one `RX;` — the exact counts.
- **Four distinct tones**: 1500.00, 1501.46, 1502.93, 1504.39 Hz, i.e. the
  1500 Hz base plus 0/1/2/3 × 1.4648 Hz. The tone spacing is right on the wire,
  not just in the constant.
- **Fired on the even UTC minute**, from an arm ~63 s earlier.
- **Total 110.6 s** against the protocol's 110.592 s.
- The safety tail ran: key-up `TA0;` at t+110,590,848 µs, then `RX;` 4.9 ms
  later (the envelope settle), then `cat_poll_set_paused(false)` — the CAT poll
  heartbeat resumed 16 ms after the burst ended.
- USB audio never faltered: 47–49 k pairs/s throughout both bursts, with FT8
  capturing and decoding concurrently the whole time.

## The one real defect found: symbol timing under load

Measuring the per-symbol `[DRY RUN t+…us]` timestamps against the ideal
682,666.67 µs grid is the whole reason those timestamps are logged, and it
immediately found something a code reading would not have.

At the original `tskIDLE_PRIORITY + 1`, **15 of 162 symbols went out late — 7
of them by more than 30 ms, worst 66.4 ms.** Every late symbol coincided with
ordinary higher-priority work (`FT8 cap:`, `uac-host: RX xport:`); no errors, no
anomaly. It is plain priority starvation, the same one CLAUDE.md's #199 note
describes for the FT8 tasks: priority 1 sits below `fft_task` (4), `cat_poll`
and `cat_link` (5) and `audio_task` (6).

Raising the worker to **priority 5** fixed it. Controlled A/B, same firmware
otherwise, same concurrent FT8 load (224 decodes during the "after" burst vs
198 during the "before" one — if anything the fixed run was busier):

| | prio 1 | prio 5 |
|---|---|---|
| median error | −464 µs | −688 µs |
| p90 | +896 µs | −336 µs |
| **worst symbol** | **+66.4 ms** | **+9.0 ms** |
| symbols >10 ms late | 13 / 162 | 0 |
| symbols >30 ms late | 7 / 162 | 0 |

5 is deliberate, not "as high as possible": it puts the burst with the CAT
tasks, which is what it is, and stays strictly below `audio_task` (6) so the USB
isochronous pump keeps the margin whose loss cost 170–350 ms of audio per slot
in #51.

**The error never accumulated even before the fix** — mean measured period
682,606 µs against an ideal 682,667 — because `run_burst()` sleeps to absolute
`t0 + i × period` targets rather than adding delays. That is the design being
right; the priority was the part that was wrong.

The small residual −688 µs median is `pdMS_TO_TICKS()` truncating microseconds
to whole 1 ms ticks, so the task wakes up to 1 ms early. Harmless, and correctable
with a short busy-wait at the end of `sleep_until()` if it is ever worth it.

### A trap in reading the capture, worth keeping

The first pass at the "after" numbers came out as a median of exactly one symbol
period, which is nonsense of a very specific kind. The cause: the serial capture
had **dropped a single log line**, and the analysis was indexing symbols by row
number, so every symbol after the gap was compared against its neighbour's ideal
time. The firmware was fine — its own `WSPR TX [161/162]` progress marker and
`burst complete (110.6 s)` prove all 162 ran.

Two things came out of that and both are now the method: derive each symbol's
index from **its own timestamp** (`round(t / period)`), never from its position
in the file; and when a statistic lands suspiciously close to a system constant,
check the parse before believing the result.

## What the dry run does and does not prove

**Proven**: message → 162 symbols → correctly spaced tones → correctly timed
command sequence → clean key-up and release, under realistic concurrent load, on
real silicon and real scheduling.

**Not proven, and not provable this way**:
- That the QMX responds correctly to a `TA<freq>;` every 682 ms for 110 s. In a
  dry run **no CAT bytes are sent at all**, so nothing here exercises the radio,
  the CDC pipe, or the QMX's own tone-change latency. FT8 TX sends `TA` at a
  160 ms cadence and is on-air validated, so the mechanism is not in doubt — but
  the 110 s continuous key-down is a different duty cycle from FT8's ~12.6 s.
- That anyone can decode it. Only an on-air test with a WSPRnet spot proves the
  chain end to end.

Because the burst sends nothing, the **only** part of a dry run that needs a
radio at all is the arm-time Digi-mode confirmation.

## ON THE AIR — 2026-08-23, 21:28 UTC

First WSPR transmission from this firmware, supervised, one burst, operator
awake and watching the radio. `OZ1LAV JO65 37` on 14.09710 MHz (dial 14.0956 +
1500 Hz audio), 5 W.

**Decoded by 50 receiving stations**, from 913 km to 15,663 km:

| reporter | grid | SNR | km |
|---|---|---|---|
| VK5WA/2 | QG50nf | −19 | 15,663 |
| VK5ARG | PF95ht | −6 | 15,280 |
| WA2TP | FN30lu | −7 | 6,152 |
| EB5TC | IM99tk | −12 | 2,038 |
| F1ZNO | JN14sc | −8 | 1,431 |
| GW2HFR | IO83ib | −4 | 1,086 |
| G4HZX | IO91xk | −6 | 969 |
| HB9VQQ | JN47kh | −9 | 951 |
| G3VGZ | IO94im | −6 | 913 |
| …40 more | | | |

What each column independently confirms:

- **Grid `JO65` and power `+37 dBm`, identical on all 50 rows.** The message
  survived packing, convolutional encoding, interleaving, sync merge, tone
  mapping, 110.6 s of CAT stepping and the ionosphere, and came back bit-exact
  from 50 decoders that have never seen our code.
- **Frequency 14.097108–14.097111 MHz** against a predicted 14.09710 — within
  about 10 Hz, which is the QMX's own calibration, not ours.
- **Drift 0 on every single spot, no exceptions.** This is the result worth
  keeping. Drift is what a receiver measures when a transmitter wanders during
  the transmission, so 50 independent stations all reporting zero is external
  confirmation of the tone stepping AND the symbol timing under real RF - the
  thing a dry run structurally cannot prove.

Also cleared, since these were the open questions from the dry-run section
above: **zero `send failed`** across all 162 real CAT writes, the QMX accepted a
`TA<freq>;` every 682 ms for 110 s without complaint, the 110 s continuous
key-down completed with no thermal or SWR event, and the CAT poll heartbeat
resumed 2 ms after the burst. The `[n/162]` markers landed at 13,651–13,656 ms
per 20 symbols against an ideal 13,653.3, so the timing held with real blocking
CDC writes and not merely when logging.

⚠ **A live build loses the per-symbol telemetry.** `tx_cmd()` only logs its
`[DRY RUN t+…us]` line when `WSPR_TX_SEND_LIVE` is 0, so a live burst gives only
the every-20-symbols progress markers. Fine here - the external drift figure is
better evidence than our own timestamps could ever be - but if per-symbol timing
ever needs measuring *on air*, that logging has to be made unconditional first.

⚠ The build that transmitted was a **temporary local edit** of
`WSPR_TX_SEND_LIVE` to 1, reverted in the source immediately after flashing.
Nothing in git has ever had it set to 1, and going on air stays a deliberate,
supervised act rather than a build-time default.

## Start offset: WSPR begins one second INTO the even minute

Caught while preparing the on-air test, and it would have been invisible from
our side - we would have transmitted perfectly and simply been early.

`wspr_tx_seconds_until_next_slot()` targeted `sec_in_minute == 0`. The
convention is +1 s: 110.6 s of signal inside a 120 s window with the slack
mostly at the end.

Rather than take that from recollection, it was **measured against real
traffic**. The five stations in the reference WAV (recorded from the even
minute) start at:

| W5BIT | KI7CI | WD4LHT | W3HH | ND6P |
|---|---|---|---|---|
| 1.109 s | 1.515 s | 1.621 s | 1.813 s | 2.133 s |

A clear floor at ~1.1 s with each station's own clock error stacked above it -
so the convention is real, and firing at `:00` would have put us ~1.6 s ahead of
the population every receiver searches around. `WSPR_TX_START_OFFSET_MS` (1000)
was added before the on-air test, and the 50 spots say the resulting alignment
is right.

The same reference WAV also independently confirmed that **37 dBm is a legal
WSPR power quantization** - KI7CI transmits at exactly that - so the power we
encoded was checked against real traffic rather than a remembered table.

## A dry run no longer needs a radio

`wspr_tx_arm()` refused to arm unless the QMX confirmed Digi mode. In a dry-run
build that check guards an action that cannot happen: `tx_cmd()` sends zero
bytes, so the burst cannot key anything or reach the radio at all.

Left strict it made the engine untestable exactly when bench time is cheapest.
This Tab5 wedges its QMX on **every** reflash (#74 - confirmed deterministic by
the operator: "it will never survive on its own"), so after any firmware change
there is no radio until someone power-cycles it by hand, and all the timing work
that needs neither radio nor antenna sat blocked behind that.

Now a dry-run build logs a warning and arms anyway when `cat_is_ready()` is
false. **A live build keeps the check unconditionally** - there it is the real
thing, the one that stops a burst going out in the wrong mode.
