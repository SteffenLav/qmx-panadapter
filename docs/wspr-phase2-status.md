# WSPR Phase 2 — TX engine status

Companion to `wspr-phase1-status.md` (RX codec + decoder). This file covers
`main/wspr_tx.c` / `.h`: building a WSPR transmission and driving it out as a
timed CAT tone sequence.

**Nothing in this file has been on the air.** `WSPR_TX_SEND_LIVE` defaults to
0 and every result below is from a dry run, where `tx_cmd()` logs the command
it would have sent and sends nothing. That distinction is load-bearing when
reading the timing numbers: see "what the dry run does and does not prove".

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
