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

### Session results — 2026-08-03/04, QMX+ on firmware 1_04_004

- Items 4, 5, 6: **PASS.** `TB;` polls cleanly at 1 Hz and decodes (it happily
  decodes band noise into stray characters — a "quiet ticker" was the wrong
  expectation, harmless). `KS` get/set works and the radio honours it. The new
  poll phases never garbled the FA/MD/FW rotation.
- Items 1, 2, 3: **CAT `KY` does not key this radio, and the fault is NOT on
  our side.** `KY <text>;` is accepted (no `?;`) and the SET itself answers
  `KY1` — the buffer demonstrably took the characters — and then nothing is
  ever sent.

**Choose the right observable — two of the three are useless.** This cost two
evenings and one wrong conclusion, so it is worth stating plainly:

| Observable | After CAT `KY` | After the QMX's OWN front-panel send | Verdict |
|---|---|---|---|
| Sidetone in headphones | silent | **silent** | **useless** — practice mode suppresses it for *any* message send |
| `M` indicator on the LCD | absent | **absent** | **useless** — same |
| **`TB` t-field** (chars left) | **0, never moves** | **counts down (`TB100;`)** | **the only reliable one** |

- The sidetone/`M` silence made both paths look identical and briefly produced
  the wrong conclusion that CAT `KY` was fine. The `TB` t-field separates them
  cleanly: the radio's own keyer reports itself running; the CAT-initiated
  send never does. **`TB`'s t-field is the ground truth for "is it keying".**
- **`KY;` GET readings depend on buffer state and must NOT be trusted as a
  baseline.** An intermediate write-up of this session claimed "an idle QMX
  answers `KY0;`" — **that was measured on a radio whose buffer had been
  stuffed by earlier un-keyed sends within the same power cycle.** After a QMX
  power cycle the same query answers `KY2;` ("no message being sent, buffer
  empty"), which is the correct idle value, and makes the earlier `KY0`
  ("sending in progress") and the repeated `KY1` (">75% full" after a 24-char
  message) read as a radio stuck in a sending state it never completed. Power
  cycle the QMX before taking any `KY;` baseline.
- **`?;` rejections appear too, and are specific to our sends** (6 of 6 `?;`
  responses in the capture window followed a CW send; none occurred anywhere
  else in the CAT stream). The manual defines `?;` as "the attempted message
  would overflow the transmit buffer; in this case it is canceled" — but it
  fires on a freshly power-cycled radio with a provably empty buffer, in CW
  mode, with a 9-character message. **Unexplained.** The same plain command
  was accepted earlier in the session, so some invisible radio state governs
  it.
- Ruled out, so nobody re-tests them: Practice mode genuinely on (`P` shown),
  keyer mode Iambic A, `KS` 12, sidetone volume 50, paddle jack empty, IQ mode
  on and off, Band Config TX enabled, radio confirmed in CW mode (`MD3`,
  heartbeat `mode=CW`) at the moment of rejection. Confound that cost one run:
  a factory reset empties all 12 stored messages, and sending an empty one is
  indistinguishable from failure — put text in message 1 before using the
  front-panel send as a control.
- **The TS480-compat path was NEVER ACTUALLY TESTED** — a second confound.
  The radio's `KY TS480` item stayed at `NO` (the instruction to change it was
  written as a menu-navigation list and read as a list of expected values), so
  the padded 24-character commands went to a radio in plain mode and their
  `?;` rejections mean only "wrong format for the current mode". The probe
  code is still in `cat.c` behind `s_ky_ts480_compat` (default false) and both
  framings it tries are ready to go. **The radio setting and our format must
  always agree: plain form ↔ `NO`, padded form ↔ `YES`.**

## ⭐ RESOLVED 2026-09-30: `KY` keys the radio

The operator tried it on air and heard it: *"the cw is keyed clearly on the
antenna - i have tried it and it works"*. Everything below this heading was
written while the conclusion was "KY does not key", and it is kept because the
reasoning is instructive, not because the conclusion stands.

**Why two evenings got it wrong:** the whole negative case rested on no
sidetone and no LCD `M` indicator, and **practice mode suppresses both of them
for message sending** — a fact this very document records a few paragraphs up.
So the instruments that reported "nothing happened" were switched off. A
negative result needs its instruments proved alive exactly as much as a
positive one does.

**Consequently items 2 and 3 below are moot:** the TS480-compat path
(`s_ky_ts480_compat`) is not needed — plain form against `KY TS480 = NO`
works — and the groups.io question should NOT be sent, since its premise
("CAT KY is accepted and never keyed") is false.

**What is still genuinely open:** there is no way to abort a message in
flight. The manual documents no `KY` abort in TS480-compat-OFF mode, so a sent
message runs to the end of the radio's 80-character buffer. That is the next
hardware question.

---

**Open, in priority order (HISTORICAL — see the section above):**

1. **Practice mode OFF + dummy load** — now the FIRST thing to try, not the
   last. Every remaining question (does `KY` key at all, what aborts it, what
   the `?;` depends on) is answerable in one minute with a load attached, and
   none of them are answerable without one. Two evenings went into working
   around the missing dummy load; that was the wrong trade.
2. **Then, if `KY` still misbehaves, the TS480-compat path** — genuinely
   untested (see above). Set `KY TS480` = `YES` on the radio *and* flip
   `s_ky_ts480_compat` to true; they must match.
3. **groups.io question**, precise and evidenced: *"On 1_04_004 in Practice
   mode, CAT `KY` is either accepted and never keyed (`TB` t-field stays 0)
   or rejected with `?;` on an empty buffer in CW mode — while the radio's own
   front-panel stored-message send does key and does move the t-field. What
   does CAT `KY` need that the front panel does not?"* The SideKar products
   key real TX through this exact command daily, so a missing precondition is
   far more likely than a plain firmware bug.

**Status for release notes: the CW page keys the radio (operator-confirmed off
air, 2026-09-30). What it cannot do is abort a message once sent.**
Do not describe CW transmit as working until item 1 or 2 above passes.
