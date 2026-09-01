# WSPR (TX + RX, own page) — scoping document

Branch: `feat/wspr-page` (local only, like `feat/cw-page` — not pushed, not in
any release until it's ready). This file is the plan; nothing here is
implemented yet.

## What WSPR actually is, and why it is NOT "FT8 with different numbers"

Every design decision below follows from these facts, so they come first.

| | FT8 / FT4 (what we have) | WSPR |
|---|---|---|
| Slot length | 15 s / 7.5 s | **120 s**, transmission itself ~110.6 s |
| Slot boundary | every slot | **even UTC minutes only** (:00, :02, :04…) |
| Tones | 8-FSK, 6.25 Hz spacing | **4-FSK, 1.4648 Hz spacing** (~4.4 Hz total bandwidth) |
| FEC | LDPC (ft8_lib, vendored) | **convolutional, K=32, decoded by Fano sequential decoding** — plain Viterbi is not feasible at K=32 (2^31 states); WSJT-X's own `wsprd` uses Fano for exactly this reason |
| Protocol shape | two-way QSO (CQ → report → RR73 → 73) | **one-way beacon**. No exchange, no `ft8_qso.c`-style state machine applies at all |
| Content | callsign + grid + report | callsign + 4-char grid + power (dBm), packed into a fixed 50-bit type-1 message |
| Who transmits | whoever is worked | **most stations only listen**; TX is a duty-cycle choice (e.g. 20% of slots), not an exchange |
| Reporting network | PSK Reporter (already integrated, `net/pskreporter.c`) | **wsprnet.org** — different site, different submission format, not yet looked at |

The one thing WSPR and FT8 genuinely share on this board: both ride the same
USB IQ audio stream and the same underlying capture/FFT primitives
(`dsps_fft2r`, already proven in `dsp.c`). Everything above the FFT is
effectively a new subsystem.

## The single biggest open question, before anything else: CAT tone resolution

Our FT8/FT4 TX works by literally re-tuning the radio's audio tone every
symbol via CAT (`TX;` → 79× `TA<freq>;` at 160 ms → `TA0;` → `RX;` — see
CLAUDE.md's "FT8 TX" section). That is NOT true GFSK; it is a hard tone jump
at each symbol boundary, and it demonstrably works well enough for FT8's
6.25 Hz spacing.

WSPR's tone spacing is **1.4648 Hz** — under a third of FT8's. **Before
writing a line of WSPR TX code, check whether `TA<freq>;` even accepts
sub-Hz resolution.** If the CAT command only takes whole Hz (typical for a
Kenwood-style command set), all four WSPR tones could round into the same
1 Hz bucket and the transmission would carry no information at all. This is
a five-minute check against the QMX CAT manual and is the first thing to do
on this branch — everything else about TX depends on the answer.

**If CAT resolution is insufficient, TX is not lost — it's just not ours to
build.** The QMX already has a native WSPR beacon (QRP Labs' "Virtual U3S"
mode, 1_04+ firmware, noted in CLAUDE.md's "QMX `1_04` beta firmware"
section) — reachable today through the Radio Menus feature we just shipped
(#147). That may simply be the right production answer for TX: no firmware
work, no audio-quality risk, just point the operator at a menu we already
expose. Decode (RX) is the part with no such shortcut.

## RX: the real effort, and where it should start

**Do not start with firmware.** This project's own pattern for a
protocol-encoding risk is a host-side harness that links the real code
before anything touches a task or an ISR — `test/ft8_cq_encode_harness.c`,
`test/lotw_harness.c`, `test/ansi_term_harness.c` are all this shape. WSPR
decode is a harder version of the same idea, and the harness should come
first for the same reason: it is much cheaper to discover the algorithm is
wrong on a PC than after it's wired into a task.

**Phase 0 — research (no code):**
1. Confirm CAT tone resolution (above).
2. Pull the WSPR type-1 message packing, the 162-bit sync vector, and the
   convolutional code's generator polynomials from an authoritative source
   (WSJT-X's own source, or the K1JT WSPR protocol spec) — the same
   discipline already used for the ARRL Field Day mode ("verified byte-for-
   byte against WSJT-X's own `packjt77.f90`"). Do not guess these constants;
   a wrong generator polynomial produces a decoder that compiles, runs, and
   never decodes anything, which is a very expensive thing to debug blind.
3. Decide on the Fano decoder specifically — it is the part every fast/lazy
   WSPR implementation gets wrong (full Viterbi is a common but infeasible
   first instinct; a decoder that's "mostly right" quietly loses the weakest
   signals, which is the entire point of the mode).

**Phase 1 — host harness:** a portable, no-ESP-deps decoder
(`main/wspr_decode.c` + `test/wspr_decode_harness.c`, following the existing
convention) built and tested entirely on the PC against **real captured
WSPR audio** — there are public WAV archives of real WSPR traffic
specifically for testing decoders against. Get this decoding real, known
signals correctly before it ever touches the device. This phase alone is
probably the majority of the total effort.

**Phase 2 — device integration:**
- Capture: extend the `dsp_ft8_capture_begin/progress/finish` streaming
  pattern (already anchors to a UTC boundary and already supports a
  target-sample count larger than one call) to a ~110.6 s window instead of
  15 s. The buffer itself is not the hard part — PSRAM has the room (this
  board runs with mid-teens of MB free even under load) — but the CAPTURE
  needs to survive across an interval roughly 8x longer than FT8's, sharing
  the same audio ring buffer, same USB ISO pipeline that #51 depends on.
  Watch for the FT8 capture-window drift bug's whole family (CLAUDE.md's
  "FT8 capture window must be UTC-boundary-capped") — a WSPR window is 8x
  more sensitive to the same class of clock drift.
- Slot loop: **new**, not a variant of `ft8_test.c`'s. WSPR is RX-mostly, no
  QSO, so there is no state machine to port from `ft8_qso.c` — this is
  closer to "capture every even minute, decode, publish spots" plus an
  independent, occasional TX arm on a duty-cycle timer. Much simpler than
  the FT8 slot loop in shape, precisely because there's no exchange to track.
- TX (only if Phase 0's CAT-resolution check allows it): reuse the
  arm/burst/CAT-tone-step mechanics from `ft8_tx.c` at 162 symbols /
  ~682 ms cadence instead of 79 symbols / 160 ms, gated the same way FT8 TX
  is (digi-mode pre-flight, `cat_poll_set_paused`, the tail `TA0;`/`RX;`).

**Phase 3 — UI (own page, per your ask):**
- A new screen, sibling to the FT8 screen and the CW page, not a mode toggle
  bolted onto either.
- Spot list: callsign, grid, power (dBm), SNR, drift (Hz over the interval),
  distance/bearing (reuse `util/maidenhead.c`, already used by FT8's KM/BRG
  columns) — no QSO columns, no worked-before, none of FT8's exchange
  furniture, since there's no exchange.
- TX control: duty-cycle percsetting or a manual "transmit next slot" —
  much simpler than FT8's tone-picker/CQ-preset machinery, since WSPR has no
  callable frequency choice, no reply logic, and no operator-facing message
  editor (the type-1 message is just callsign + grid + power, all of which
  already exist as settings).
- Web parity: a `/api/wspr`-style read endpoint mirroring `/api/decodes`'
  shape is cheap once the device side exists, and should probably ship in
  the same pass rather than be a separate follow-up — this project's own
  "web/Tab5 parity" rule already expects it.

**Phase 4 — reporting (deferred, genuinely optional for a first cut):**
wsprnet.org spot submission is a different site and a different submission
format from PSK Reporter's IPFIX/UDP — this needs its own research pass
(read `wsprnet.org`'s actual spot-submission mechanism) before assuming
`net/pskreporter.c` is any kind of template. Reasonable to ship Phase 1-3
first and decide separately whether reporting is worth it.

## Effort read

Phase 1 (the decoder itself, proven on a PC against real recordings) is the
long pole — a correct Fano-decoded WSPR RX chain is a legitimately hard DSP
problem, harder than anything in the FT8 path because ft8_lib already
existed as vendored, mature code; there is no equivalent library to lean on
here. Phases 2-3 are comparable in size to a mid-sized existing feature
(roughly the shape of the CW page's own effort) once Phase 1 is solid.
Realistic expectation: this is a multi-week effort spread over many
sessions, not a single sprint, and Phase 0-1 should be complete and
independently convincing (decoding real WSPR WAV captures correctly) before
any firmware integration begins.

## Recommended first concrete step

Do Phase 0 item 1 (the CAT tone-resolution check) first — it's cheap and it
determines whether TX is "wire up what already exists" or "point at the
QMX's own Virtual U3S mode instead." Then start Phase 1 with the host
harness, exactly like every other protocol-risk in this codebase has been
de-risked before it touched a task.
