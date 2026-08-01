# CW Page — design exploration (2026-08-01)

Status: DESIGN, nothing built. Companion to TODO #4/#4b. CAT surface confirmed
2026-08-01: `KY` (send CW text, radio does all keying), `KS` (keyer WPM get/set),
`TB;` (read+empty the QMX's own CW decode buffer, plus KY TX-progress) — all
present in 1_03_002 AND 1_04, no firmware gating.

## What our platform has that no CW product combines

Every popular CW tool has a subset; nothing has all of it in one box on the
radio itself:

- A live FFT/waterfall of the whole passband (CW Skimmer needs a PC + SDR)
- A touch screen with a working gesture language
- A full QSO state machine, pileup tracker, grey-list, worked-before cache,
  dupe guard (built for FT8, mode-agnostic in spirit)
- ADIF logging + QRZ/eQSL/LoTW upload with no PC
- DXCC/country/distance/bearing lookup tables already in flash
- WiFi (RBN queries possible), GPS/RTC time
- A practice-simulation philosophy (ft8_sim) users already know

The design theme that falls out: **give CW the FT8-grade situational awareness
and assistance this project already built once** — while keeping the operator's
hand on the key. Nobody offers that on a standalone rig-attached screen.

## Research notes — what the popular tools do

**N1MM Logger+ (contest keying)** — function-key stored messages with macro
substitution (`{MYCALL}`, the entered callsign, serial numbers), and **ESM
("Enter Sends Message")**: one key steps through the whole QSO — CQ → exchange
→ TU — choosing the right message from QSO state. The highlighted next-message
label teaches the flow. This is the single most-loved CW-contest UX feature.

**CW Skimmer** — Bayesian decode of every CW signal in the passband at once,
callsign tags rendered next to the traces on the waterfall. The RBN is built
out of these.

**Reverse Beacon Network** — skimmers worldwide report every CQ heard:
callsign, SNR, **and measured WPM**. Calling "CQ POTA" gets you auto-spotted
to POTA via RBN; W5ZO-style antenna A/B testing via your own RBN numbers is a
standard shack ritual.

**Morserino-32** — the learning benchmark: Koch trainer, echo trainer
(hear→key back→instant feedback), Farnsworth spacing, adaptive focus on weak
characters. (QRP Labs sells it; our users know it.)

**POTA CW practice** (docs.pota.app CW guide, QRPer) — the real-world
workflow: memory keyer slots for CQ + exchange, callsign + RST + state,
"update the keyer for each new park or you send yesterday's park number",
abbreviated exchanges when a pileup builds. QRPer literally asks "could you
activate a park in CW only using a decoder and memory keyer?" — the answer is
yes, people do, and that person is our first-class user.

## Layout concept

Third carousel stop (Panadapter ↔ FT8 ↔ CW), same left-pane + main-area
skeleton as the FT8 page so the gesture language transfers. Unlike FT8, the
spectrum AND waterfall stay live — CW is a tuning mode. Two densities
(mockups reviewed + approved by the operator 2026-08-01):

**Shack layout** — decode stream dominant:

```
+--------------------------------------------------------------+
| top bar (unchanged): freq | band | CW | 300 Hz | S-meter     |
+--------+-----------------------------------------------------+
| NEXT   | spectrum (live) -- amber VFO line + ZERO-BEAT       |
| sends: |   needle at the CW pitch (tier 2)                   |
| <msg>  +-----------------------------------------------------+
| [green]| waterfall (live) -- callsign+wpm skimmer tags       |
|--------|   (tier 3 research spike; untagged at first)        |
| M1  M2 +-------------------------------+---------------------+
| M3  M4 | decode ticker (TB;), rolling, | station card:       |
|--------| timestamped; our TX inter-    | W1AW - USA          |
| -18wpm+| leaved dimmer; recognized     | 6213 km - 292 deg   |
| match  | callsign highlighted amber    | worked 20m / new 40m|
| TX sts |                               | their speed: 22 wpm |
+--------+-------------------------------+---------------------+
| bottom bar (unchanged)                                       |
+--------------------------------------------------------------+
```

- Left pane top: the ESM **NEXT** button (green, always states what it sends).
- Under it: M1-M6 memory grid, WPM stepper with a "match their N" chip
  (tier 2), TX/decode status at the bottom.
- Station card pops when a callsign is captured from the ticker (tier 2).

**POTA layout** — buttons dominant, one-thumb glove operation:

```
+--------------------------------------------------------------+
| top bar (unchanged)                                          |
+--------------------------------------------------------------+
| thin spectrum strip (tuning only), no waterfall              |
+--------------------------------------+-----------------------+
|  NEXT sends                          | Park: US-1211 (tap    |
|  CQ POTA DE OZ1LAV K                 |   to edit)            |
|  [giant green, ~1/3 of screen]       | QSOs: 7 - 3 to go     |
+------------+------------+------------+------------+----------+
| CQ POTA    | {CALL} 599 | TU 73 +LOG | -  16  +  WPM        |
+------------+------------+------------+------------+----------+
| decode ticker, 1-2 lines: OZ1LAV DE K4XYZ 599 TN BK          |
+--------------------------------------------------------------+
| bottom bar                                                   |
+--------------------------------------------------------------+
```

- Park number edit is one tap (the documented #1 POTA keyer failure is
  sending yesterday's park); activation counter reuses the green-at-10 rule.
- Density toggle = one button on the page.

**ESM ladder (the one NEXT button through a whole QSO):**
`CQ POTA DE OZ1LAV K` → (K4XYZ calls in, captured) → `K4XYZ 599 599 GA BK`
→ (their `599 TN BK` decoded) → `TU 73 DE OZ1LAV EE` → one tap = logged
(K4XYZ, 40m, 599/599) → back to CQ. Every state is the same physical button,
relabeled from QSO state; the operator only ever decides "yes, send that".

**RBN self-view card** (tier 3): after a CQ — "14 skimmers - 4 continents /
EU best 32 dB (SM7IUN) / NA best 18 dB / your keying reads 17.8 WPM / POTA
auto-spot: confirmed". Batched WiFi fetch, WS paused (the LoTW lesson).

**Copy trainer overlay** (tier 3): the newest ticker line hidden behind a
redaction bar with an adjustable delay — copy the live band by ear, tap
reveal, self-score; per-character weak-spot stats over time ("82% on
letters - Q, Y, 5 need work").

## Feature tiers

### Tier 1 — table stakes (Phase 1, ship first)

- **Memory keyer**: 6 slots, edited like the FT8 CQ presets (same modal
  pattern, same NVS style). Sent via `KY`; radio does timing/envelope.
- **Macro variables** in memories: `{MYCALL}` `{CALL}` (captured, see Tier 2)
  `{RST}` `{PARK}` `{SERIAL}` (auto-increment) `{WPM}`. N1MM's vocabulary,
  trimmed to what POTA/casual use needs.
- **WPM control**: read-back-on-open slider or +/- steps via `KS` (the QMX
  volume pattern, including "the number matches the radio").
- **TX progress + abort**: `TB`'s `t` field = chars remaining; big red
  tap-to-abort while sending (KY buffer can be… checked: worst case abort =
  mode bounce; verify on hardware what cancels a KY send).
- **Decode ticker**: 1 Hz `TB;` poll phase in cat.c (new poll phase, same
  pattern as the tune poll), scrolling text under the spectrum.
- **Park number field**: one-tap edit at the top of the POTA layout — the
  documented #1 field failure is sending yesterday's park.

### Tier 2 — the smart layer (Phase 2, where it gets interesting)

- **ESM: one big NEXT button.** The killer feature to steal. The button
  always shows what it will send ("CQ POTA…", then "{CALL} 599 599", then
  "TU 73 {PARK}") and one tap advances the ladder. QSO state comes from what
  was sent + what TB decoded. Contest ops get muscle-memory ESM; a gloved
  POTA activator gets a one-thumb QSO.
- **Callsign capture**: scan TB text for a callsign pattern (we already have
  callsign validation from the PSK Reporter guard + hash work); the captured
  call fills `{CALL}`, lights up an **info card: country, bearing, distance,
  worked-before badge, dupe-on-this-band warning** — the FT8 decode-list
  enrichment, applied to CW. No CW product on the market does this on-radio.
- **One-tap log**: after TU, the log entry is pre-built (call, RST both ways,
  band, freq, park). Tap = ADIF + the whole existing upload pipeline. The
  POTA green-at-10 counter already exists in the viewer.
- **Zero-beat assistant**: we own the FFT — show a fine tuning needle for
  "their tone vs your sidetone offset" when a CW signal is near the passband
  center; tap-to-tune already gets coarse. Rigs bury this in a menu; ours can
  be a live visual.
- **Speed matching**: measure the tuned signal's dit rate from FFT bin
  on/off timing (pure DSP on data we already have — the QMX decoder doesn't
  report WPM over CAT) and offer "match their 17 WPM" as one tap on the WPM
  control. Answering faster than you can copy is the classic beginner trap;
  answering someone at *their* speed is universal CW courtesy.

### Tier 3 — nobody-has-this (Phase 3 candidates, pick winners later)

- **CW copilot (assisted QSO for the not-yet-CW-ham).** The honest version of
  what the guided ladder already implies: the decode ticker + pattern hints
  ("that was likely a report", "they sent your call") + the NEXT button means
  someone who cannot yet copy by ear can complete a real, legal, polite CW
  QSO — the same way the FT8 page carries operators who couldn't hand-key a
  QSO ladder. Controversial in the same way decoders always are; also exactly
  how a lot of people bridge into CW. The operator is the target user here.
- **Real-signal copy trainer.** Morserino can't do this: a practice overlay
  where the decode ticker is *hidden and delayed* — you copy the live band by
  ear, type/tap what you heard (or just mentally copy), then reveal. Uses
  real QSB/QRM signals as training material. Track per-character hit rate
  Koch-style over time. A trainer fed by the actual band is genuinely novel.
- **RBN self-view.** After a CQ, query reversebeacon.net over WiFi and show:
  how many skimmers heard you, best SNR per continent, and **what WPM the
  world measured your keying at**. Shack: instant antenna/prop A/B. POTA:
  doubles as confirmation your CQ triggered RBN→POTA auto-spotting. (Needs
  the RBN telnet/API surface scoped; batched like PSK Reporter, WS paused
  during fetch per the LoTW lesson.)
- **Waterfall skimmer tags (mini CW Skimmer).** Multi-signal decode with
  callsign tags on the waterfall. Full 700-signal Skimmer is a PC fantasy,
  but 2–4 parallel Goertzel decoders on the strongest peaks is plausible on
  the P4 — as long as it lives OUTSIDE the FT8 path and respects every
  CLAUDE.md lesson about task priorities and internal RAM. Treat as a
  research spike with a kill criterion, not a promise.
- **Phantom CW practice (sim-mode analog).** Inject phantom stations' text
  straight into the decode ticker (the ft8_sim trick — no audio needed, no
  radio keyed) so the whole ESM/capture/log workflow can be rehearsed at the
  kitchen table. Cheap, consistent with the project's practice philosophy,
  and it exercises the UI for development too.

### Explicitly out (for now)

- **CW audio to the Tab5 speaker** — stays shelved until the cw_audio task
  priority problem is fixed (four releases of FT8 yield say so). The QMX's
  own headphone jack covers listening.
- **On-device Koch/echo trainer with keying** — Morserino owns this and QRP
  Labs sells it; don't rebuild it worse. Our trainer angle is the real-band
  copy trainer above, which Morserino cannot do.
- **Straight-key/paddle input via Tab5** — the QMX has paddle inputs;
  duplicating them adds latency for nothing.

## Hardware verification checklist (first bench session with a QMX)

1. `KY HELLO;` in CW mode — keys? Envelope OK? What happens in USB mode?
2. TS480-compat setting: which variant does a stock QMX ship with; does `KY;`
   get match the manual for both.
3. How to ABORT a KY send mid-message (RX;? mode change? buffer behavior).
4. `TB;` while the QMX CW decoder is on: decode quality at real signal
   levels; does it decode only the CW-filter passband center; buffer pacing.
5. `KS` set — does the QMX LCD reflect it; range limits.
6. Interaction with the FA/MD/FW poll (new poll phases must not garble).
