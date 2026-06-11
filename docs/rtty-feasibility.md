# RTTY mode — feasibility & implementation plan

Scoping pass for adding RTTY (45.45 baud Baudot/ITA2) as a third digital mode
alongside FT8 and JS8, prompted by the new M5Stack Tab5 Keyboard accessory —
RTTY is a live-typing mode, and the keyboard is what makes it practical on
this hardware. Based on the QMX CAT/operation manuals (`docs/qmx-reference/`),
the M5Stack Tab5 Keyboard docs, the public-domain ITA2/Baudot standard, and
this project's existing `main/cat/`, `main/ft8_*`, `main/audio/`, `main/dsp/`.
No code written yet.

## TL;DR

Where JS8 was "FT8 with different tables" (high reuse via `ft8_lib`'s existing
multi-protocol abstraction), RTTY is a different animal: a continuous,
asynchronous 2-tone FSK character stream with no LDPC, no sync preamble, and
no fixed block length. **Almost nothing in `ft8_lib` applies** — RTTY needs a
new, parallel DSP pipeline (dual-tone demod + Baudot codec), not a new branch
in the existing one.

The picture splits cleanly in two:

- **RX (decode)** is self-contained, lower-risk, and useful on its own — RTTY
  is everywhere on HF (contests, nets), so live signals to test against are
  abundant. New code, but conceptually simple: no FEC, no tables to
  transcribe, just tone-tracking and a 60-year-old character code.
- **TX** reuses QMX's existing `MD6`/`TA<freq>;` mechanism — the same one FT8
  TX already uses — but at **22 ms/bit vs FT8's 160 ms/symbol**, ~7x tighter.
  Whether QMX's firmware keeps up at that rate is untested and
  hardware-dependent — the single biggest unknown in this document.

The keyboard itself is an **I2C accessory, not USB** — good news for the
existing UAC+CDC-ACM USB host setup — but it's a brand-new I/O subsystem for
this project (first physical keyboard, first non-display I2C peripheral).

**Estimate: RX-only ≈ 3.5–4 sessions** (a complete, shippable "RTTY monitor").
**Full RX+TX+keyboard ≈ 6.5–8.5 sessions**, gated on a half-day TX-timing bench
test. Bigger and riskier than JS8's 5–7 sessions, mainly because there's no
`ft8_lib`-equivalent to lean on.

---

## Physical layer: RTTY doesn't fit `ftx_protocol_t`

| Parameter | FT8 / JS8 (`ft8_lib`, this project) | RTTY (45.45 Bd, 170 Hz shift) |
|---|---|---|
| Audio sample rate | 12000 Hz | 12000 Hz — same capture |
| Unit of transmission | 79-symbol block, fixed 15.000 s | Asynchronous character stream, unbounded length |
| Symbol/bit duration | 160 ms/symbol | **22 ms/bit** (1/45.45 s) |
| Modulation | Multi-tone GFSK, 8 tones × 6.25 Hz spacing | 2-tone FSK: mark/space, 170 Hz apart |
| Sync | 3×7 Costas array at fixed symbol positions | None — per-character start bit (space) + 1.5 stop bits (mark) |
| FEC | LDPC(174,91) FT8 / (174,87) JS8 + CRC | **None** |
| Payload encoding | 28/50/77-bit packed binary fields | 5-bit Baudot/ITA2 + LTRS/FIGS shift state |
| Decode trigger | FFT over full 15 s capture, candidate search, LDPC | Continuous dual-tone tracking + bit-clock recovery, decode per character |
| TX duration | Fixed 79×160 ms ≈ 12.6 s burst | Open-ended — operator starts/stops (PTT-style) |

`ftx_protocol_t` ([decode.c:192](../components/ft8_lib/ft8/decode.c)) branches
on Costas table / num_tones / Gray map for FT4 vs FT8 (and JS8) — all
variations on "decode one LDPC-coded block from a windowed FFT." RTTY has no
block, no LDPC, no FFT-based candidate search. It isn't a fourth
`ftx_protocol_t` value; it's a **separate pipeline running in parallel**, more
like a second consumer of the same audio ring buffer than an extension of the
existing decoder.

### Where the QMX CAT layer *does* already fit: mark/space inversion

One pleasant surprise: `MD;` already defines mode **`9` = "FSR / FSK
Reverse"** alongside `6` = "FSK" (`docs/qmx-reference/QMX_CAT_programming_manual_1.03.000.pdf`,
`MD` command). Mark/space inversion ("Normal" vs "Reverse") is a famous RTTY
operating headache — QMX's CAT layer already has a slot for it, regardless of
which mark/space convention this project picks.

---

## Reuse map

| Layer | Verdict | Notes |
|---|---|---|
| Audio capture / ring buffer (`audio.c`) | **Reuse unchanged** | RTTY wants a continuous sample stream — even less special-casing than FT8's slot capture |
| `dsp.c` FFT / spectrum | **Reuse, secondary role** | Main spectrum/waterfall keep running underneath; RTTY decode itself bypasses the block FFT |
| `decode.c` / `ldpc.c` / `crc.c` / `encode.c` / `message.c` (`ft8_lib`) | **Not applicable** | No LDPC, no Costas, no CRC, no packed-field messages in RTTY |
| `cat.c` `MD6`/`TA<freq>;` ([cat.c:628](../main/cat/cat.c)) | **Reuse mechanism, new cadence** | Same "Digi mode + audio-tone CAT command" path FT8 TX uses, at 22 ms instead of 160 ms |
| `ft8_tx.c` absolute-timer burst pattern, `cat_poll_set_paused()` | **Reuse pattern, new shape** | FT8: fixed 79-step 12.6 s burst. RTTY: open-ended toggle loop until operator stops |
| `ft8_test.c` 15 s slot loop | **Not applicable** | RTTY has no slot/parity concept — needs an always-on RX task instead |
| `ft8_qso.c` QSO state machine | **Not reusable as a ladder** | RTTY QSOs are operator-driven; see [Operating model](#operating-model-macros-not-a-ladder) |
| `ft8_screen_view.c` decode list, `ft8_status.c` | **Replaced by scrolling terminal** | RTTY RX is a live append-only text stream, not a per-slot dedup'd list |
| `ft8_tx_modal.c` confirmation modal | **Concept reusable for macros** | "Preview canned text, confirm, send" maps to RTTY macro buttons (CQ/exchange/sign-off) |
| Baudot/ITA2 codec | **New** (~80–120 lines) | Fully spec'd below — table lookups + shift-state tracking, no JS8-style bit-packing math |
| Dual-tone FSK demod + bit-clock recovery | **New** (~150–250 lines) | Goertzel (or narrow IIR) at mark/space + zero-crossing or PLL bit timing |
| Keyboard driver (I2C) | **New, no precedent** | First physical-keyboard / non-display-I2C peripheral on this project |

---

## Baudot/ITA2 (fully spec'd)

RTTY's "message format" is the 60-year-old ITA2 (CCITT-2) 5-bit code — public
domain, far more stable ground than reverse-engineering a `.cpp` file. Each
character is **1 start bit (space, 0) + 5 data bits (LSB first) + 1.5 stop
bits (mark, 1) = 7.5 bit periods ≈ 165 ms/char** at 45.45 baud (≈6 chars/sec).

Two of the 32 codes are **shift codes**, not characters — they switch the
*decoder's* interpretation of all subsequent codes until the next shift code:

| Code (b1..b5, LSB first) | LTRS | FIGS (US-TTY/ITA2) |
|---|---|---|
| 00000 | (blank) | (blank) |
| 00001 | E | 3 |
| 00010 | LF | LF |
| 00011 | A | – |
| 00100 | SP | SP |
| 00101 | S | BELL |
| 00110 | I | 8 |
| 00111 | U | 7 |
| 01000 | CR | CR |
| 01001 | D | $ |
| 01010 | R | 4 |
| 01011 | J | ' |
| 01100 | N | , |
| 01101 | F | ! |
| 01110 | C | : |
| 01111 | K | ( |
| 10000 | T | 5 |
| 10001 | Z | " |
| 10010 | L | ) |
| 10011 | W | 2 |
| 10100 | H | # |
| 10101 | Y | 6 |
| 10110 | P | 0 |
| 10111 | Q | 1 |
| 11000 | O | 9 |
| 11001 | B | ? |
| 11010 | G | & |
| **11011** | **FIGS** (shift) | **FIGS** (shift) |
| 11100 | M | . |
| 11101 | X | / |
| 11110 | V | ; |
| **11111** | **LTRS** (shift) | **LTRS** (shift) |

The digit row is the classic **QWERTYUIOP → 1234567890** mnemonic
(Q=1, W=2, E=3, R=4, T=5, Y=6, U=7, I=8, O=9, P=0) — every digit mapping above
checks out against it, which is good independent confirmation of the LTRS
column too.

- **Decoder** must track current shift state (LTRS/FIGS), initially LTRS.
  `SPACE`, `CR`, `LF` are identical in both shift states — a missed shift
  code doesn't break line framing.
- **Encoder** (TX) emits a shift code whenever the next character's table
  differs from the current state, and conventionally re-asserts LTRS after any
  word containing FIGS characters (e.g. a callsign with digits), so a missed
  shift code on the receive side self-heals.
- **USOS** ("unshift on space") — some stations treat SPACE as an implicit
  LTRS shift. Common but not universal; plan as a per-decode toggle, default
  off, not hardcoded.
- The FIGS column has minor regional variants (this is the US-TTY/ITA2 hybrid
  most ham software uses, e.g. `$`/ENQ at 01001, `#`/£ at 10100). Cross-check
  against a second source (e.g. fldigi's `rtty.cxx`) before finalizing — wrong
  FIGS punctuation is cosmetic, unlike a wrong LDPC table which breaks decode
  entirely.

---

## TX path: CAT mechanism reused, timing is new territory

Per the operation manual, "For FSK modes (WSJT-X modes including WSPR and
FT8, JS8Call, RTTY, etc) be sure to use DiGi mode" — and per the CAT manual,
`MD6` + `TA<freq>;` *is* "Digi mode." `cat.c`'s mode table already maps the
digital-soundcard family to `MD6` ([cat.c:628](../main/cat/cat.c)). **There is
no separate RTTY CAT mode to implement** — TX is the same `TA` mechanism FT8
already uses, toggling between two fixed offsets at a much faster rate:

```
TX;                          switch to transmit
loop, 22 ms cadence:
  TA<mark_or_space_hz>;      one bit's worth of mark or space tone
... (until message complete, including trailing stop bits) ...
TA0;                          key-up
RX;                           back to receive
```

Common convention: mark = 2125 Hz, space = 2295 Hz (170 Hz shift, "space
high"). `MD9`/FSR exists for stations that come in inverted (see above).

**The risk**: FT8 TX proves QMX can sustain `TA<freq>;` at 160 ms with
absolute-timer scheduling and no drift (`f55fd40`, v0.12.0). RTTY needs the
same mechanism at **22 ms — about 7x faster**. Serial transmission isn't the
bottleneck (`TA2295.0;` ≈ 10 bytes ≈ 2 ms at the 38400 baud CDC-ACM link,
[cat.c:23](../main/cat/cat.c) — ~10% of the bit period). What's unknown is
QMX's **firmware-internal** command-processing / DDS-retune latency at this
rate — a few ms of jitter is a much bigger fraction of 22 ms than of 160 ms.
This can only be answered by bench-testing real hardware (scope or SDR on the
QMX's RF output while toggling `TA` at 22 ms) — see Phase R4 below. **Do this
before writing the Baudot encoder.**

---

## The keyboard: M5Stack Tab5 Keyboard (I2C, not USB)

Per [M5Stack's docs](https://docs.m5stack.com/en/tab5/Tab5_Keyboard) and
[shop listing](https://shop.m5stack.com/products/keyboard-for-tab5), the Tab5
Keyboard is a 70-key, 14×5-matrix module with its own STM32F030C8T6 MCU,
connecting via **Tab5 Ext.Port1 — I2C** — with an interrupt pin for
low-latency key events. Three firmware modes: **Normal** (raw row/col),
**HID** (USB HID report bytes over I2C), and **Character** (key-name strings +
Ctrl/Alt modifier flags).

This is good news architecturally: **it's I2C, not USB** — sidesteps the
entire "competing with UAC+CDC-ACM for DW-GDMA channels" class of problem that
already killed a `CONFIG_LVGL_PORT_ENABLE_PPA` attempt on this project (see
CLAUDE.md). **Character mode** is the obvious starting point — it hands back
named keys + modifiers directly, no HID-report parsing or matrix-to-ASCII
table needed for v1.

The open question is the **I2C bus**. This project's existing I2C bus carries
the PI4IO expander and the ST7121/ST7123 touch controller, and CLAUDE.md
documents that bus as already finicky — 100 kHz only, "do not call
`bsp_detect_display_type()` more than once per boot," etc.
(`components/m5stack_tab5/m5stack_tab5.c`). Whether "Ext.Port1" is the *same*
I2C bus/controller or a separate one is a hardware question this project has
been burned by before — the ST7121 saga was exactly this kind of "two I2C
devices, one bus, different needs" surprise (`2b8fcaa`).
[M5Unit-KEYBOARD](https://github.com/m5stack/M5Unit-KEYBOARD) is
Arduino/M5UnitUnified — not portable to this ESP-IDF project, but useful as a
**register-map reference** (I2C address, mode-select register, key-event read
format), the same way JS8Call's source served as a protocol reference rather
than portable code.

---

## Operating model: macros, not a ladder

`ft8_qso.c`'s automated WAIT_RPT/WAIT_RR73/... state machine doesn't map to
RTTY — RTTY QSOs are operator-paced and ad-lib. What carries over is the
*concept* of canned messages from `ft8_tx_modal.c`:

| FT8/JS8 concept | RTTY equivalent |
|---|---|
| Automated state machine (`ft8_qso.c`) | None — operator drives every step |
| TX1/TX2/TX3 canned messages, confirm-and-send modal | Macro buttons: **CQ**, **Exchange** (RST + name + QTH), **Sign-off** — same "preview, confirm, send" UX, sent as a continuous Baudot stream instead of one burst |
| `fmt_report` (coarse SNR proxy) | RST report (Readability-Strength-Tone, e.g. `599`) — operator-entered, no auto-mapping |
| 15 s slot timing, parity | None — TX starts/stops on demand, arbitrary duration |
| Decode list with 60 s aging (`ft8_screen.c`) | Append-only scrolling RX terminal — every character matters, no dedup/aging |
| (n/a) | **Live keyboard passthrough during TX** — type ad-lib text, encoded to Baudot and sent character-by-character in real time |

---

## Out of scope (Tier 2)

- **AMTOR / PACTOR** and other ARQ (error-corrected) RTTY variants.
- **Non-standard baud rates** (50, 75, 100 Bd) and **shifts** (425, 850 Hz) —
  ship 45.45 Bd / 170 Hz only.
- **AFC** (automatic signal tracking) — v1 is manual tuning + a visual tuning
  aid (e.g. dual bargraph or crossed-tone display for mark/space levels).
- **USOS auto-detection**, reverse-mode auto-detection — manual toggles only.
- Keyboard **HID mode** / full key-remapping — Character mode covers RTTY's
  needs (letters, digits, punctuation, Enter, Ctrl for shortcuts).

---

## Phased plan & effort estimates

Sized against this project's full FT8 arc (`86f7638` vendor → `cc5ef1a`
v0.15.1, 2026-06-03 → 2026-06-11, ~9 days / ~6500 lines). RTTY has far less to
reuse than JS8 did, but each new piece (Baudot table, 2-tone demod) is
individually simpler than LDPC/Costas — more primitives to build, less depth
per primitive.

| Phase | Scope | Precedent | Estimate |
|---|---|---|---|
| **R1 — RX decode core** | Dual-tone (Goertzel/IIR) mark/space tracker, bit-clock recovery, Baudot/ITA2 decoder w/ shift-state; synthetic self-test (known bitstream → text) | `e0a2e3f` (new `dsp/` capture module) — but no LDPC-equivalent precedent exists | **1.5–2 sessions** |
| **R2 — RX UI** | Scrolling RX terminal widget + tuning aid (mark/space level bars); new screen wired into mode-toggle infra | `9d81ff6`/`6d4c1b8` (new screen + mode-toggle infrastructure) | **1 session** |
| **R3 — Keyboard driver** | I2C driver for Tab5 Keyboard (Character mode) → text-input buffer; resolve I2C bus sharing with touch/IO-expander | `2b8fcaa` (ST7121 I2C surprise) as cautionary precedent, not a smooth one | **1 session, +1 if bus conflict** |
| **R4 — TX timing spike** | Drive `TA<freq>;` toggling at 22 ms via CAT; scope/SDR-monitor QMX RF output for timing accuracy. **Go/no-go gate for R5** | n/a — new hardware question | **0.5 session** |
| **R5 — RTTY TX** | Baudot encoder, `TA<freq>;` mark/space sequencer (absolute-timer, open-ended), macro buttons (CQ/exchange/sign-off), live keyboard passthrough during TX | `f55fd40` (v0.12.0 manual FT8 TX via CAT `TA`) | **1.5–2 sessions**, contingent on R4 |
| **R6 — On-air validation** | RX: trivial — RTTY contests/nets are constant on 20m/40m. TX: needs a counterpart or SDR loopback | `v0.15.0`→`v0.15.1` (CQ-run + capture-window fix) | **1 session** |

**RX-only (R1–R3): ~3.5–4 sessions** — a complete, shippable "RTTY monitor,"
useful even if TX never happens. **Full R1–R6: ~6.5–8.5 sessions**, gated on
R4.

---

## Risks (in order)

1. **RTTY TX timing (22 ms `TA` cadence)** — see
   [TX path](#tx-path-cat-mechanism-reused-timing-is-new-territory). The
   single biggest unknown in this whole document; resolve with R4 before
   committing to R5.
2. **Keyboard I2C bus sharing** — Ext.Port1 vs. the existing PI4IO/touch I2C
   bus is an open hardware question on a bus this project has already found
   fragile once (ST7121).
3. **Baudot FIGS-row regional variants** — low severity (punctuation only),
   cross-check table before shipping.
4. **Signal drift / no AFC** — RTTY has no FEC to absorb frequency error; v1's
   manual tuning aid may prove too fiddly in practice, but that's a usability
   follow-up, not a blocker.
5. **On-air validation** — actually *easier* than JS8 for RX (RTTY signals are
   abundant on HF); TX validation has the same "need a counterpart" issue JS8
   has.

---

## Source references

- QMX CAT programming manual
  (`docs/qmx-reference/QMX_CAT_programming_manual_1.03.000.pdf`) — `MD`
  (modes 6/FSK, 9/FSR), `TA`, `TX`, `TQ` commands.
- QMX operation manual
  (`docs/qmx-reference/QMX_operation_manual_1.03.002.pdf`) — Digi mode setup,
  "FSK modes ... RTTY" guidance.
- M5Stack Tab5 Keyboard:
  [docs.m5stack.com/en/tab5/Tab5_Keyboard](https://docs.m5stack.com/en/tab5/Tab5_Keyboard),
  [shop listing](https://shop.m5stack.com/products/keyboard-for-tab5),
  [M5Unit-KEYBOARD](https://github.com/m5stack/M5Unit-KEYBOARD) (protocol
  reference, Arduino/M5UnitUnified — not portable).
- ITA2/Baudot code: public-domain ITU standard (CCITT-2); cross-check against
  a second implementation (e.g. fldigi `rtty.cxx`) during implementation.
- This project: [`main/cat/cat.c`](../main/cat/cat.c) (`MD6`/`TA` mechanism),
  [`main/ft8_tx.c`](../main/ft8_tx.c) (absolute-timer burst pattern),
  [`main/audio/audio.c`](../main/audio/audio.c) (ring buffer),
  [`main/dsp/dsp.c`](../main/dsp/dsp.c) (FFT/spectrum, for tuning aid).
