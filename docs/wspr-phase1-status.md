# WSPR Phase 1 — status

Companion to `docs/wspr-scope.md` and `docs/wspr-phase0-research.md`. This is
the protocol-layer slice of Phase 1: message packing, the K=32 convolutional
code, interleaving, and the Fano decoder — proven on the PC, no ESP deps, no
firmware touched yet.

## What exists

- [main/wspr_proto.h](../main/wspr_proto.h) / `.c` — WSPR type-1 message
  pack/unpack (callsign + 4-char grid + power dBm <-> the 50-bit message).
  `wspr_unpack_message()` is ported byte-identical from WSJT-X's own
  `wsprd_utils.c`; `wspr_pack_message()` is independently sourced (WsprryPi +
  wspr-tools) and proven to agree with the ported unpack via round-trip
  testing, not by construction.
- [main/wspr_fano.h](../main/wspr_fano.h) / `.c` — the convolutional
  encoder, the interleave/deinterleave permutation, sync-vector combine, and
  the Fano sequential decoder (ported near-verbatim from WSJT-X's
  `fano.c`, Phil Karn KA9Q / Joe Taylor K1JT).
- [test/wspr_codec_harness.c](../test/wspr_codec_harness.c) — the proof.
  Build/run:
  ```
  gcc -O2 -Wall -I main -o wspr_codec_harness \
      test/wspr_codec_harness.c main/wspr_proto.c main/wspr_fano.c \
      -lm && ./wspr_codec_harness
  ```
  All 5 test groups pass (pack/unpack round-trip incl. edge cases, interleave
  self-consistency, full noiseless pipeline, single- and two-symbol-error
  correction — exhaustive over all 162 positions, not sampled — and input
  validation).

## Bugs the harness caught (this is the point of building it first)

1. **Power field was missing its `+64` offset** — packed messages decoded to
   a power 64 dB too low. Caught immediately by the round-trip test.
2. **Callsign digit-anchor search was ambiguous for prefix-digit calls**
   (e.g. `4X1XX`) — "find the first digit" anchored on the wrong digit and
   produced an invalid suffix. Fixed by requiring the anchor digit's suffix
   to be letters-only, matching the standard-callsign shape the message
   format actually requires.
3. **The naive hard-decision metric table (match=1/mismatch=0) failed to
   correct even single-symbol errors** (17/24 in initial testing) — a Fano
   metric needs a *negative* expected value for a wrong path, which a plain
   match/no-match table doesn't have. Fixed to match=1/mismatch=-3 (verified
   empirically against several candidates), which now corrects 100% of
   single- and two-symbol errors (exhaustive, 162/162 and 162/162) and
   degrades gracefully beyond the code's real correction radius (3+
   simultaneous errors: not 100%, which is expected, not a bug).
4. **CORRECTED (initially misdiagnosed, see the follow-up session below).**
   I pulled a closed-form bit-reversal formula from wsprcan's source to
   cross-verify the interleave permutation, compared it against `reverse8()`
   as a 64-bit value, saw total disagreement, and wrote it up as "the
   fetched constant is wrong, almost certainly a transcription error" — an
   accusation against the source I never actually verified by hand. The
   real bug was in my own comparison: wsprcan's code declares the result as
   `unsigned char j = (...) >> 32;`, an 8-bit truncation that matters - the
   untruncated 64-bit intermediate is garbage BY DESIGN, only the low byte
   is the answer. Truncated correctly, it matches `reverse8()` exactly for
   all 256 inputs. **The interleave permutation now has a genuine
   second-source cross-check** (`test_interleave_self_consistency()` in
   `wspr_codec_harness.c`), and it passes. Kept here instead of quietly
   fixed, because "the web source was wrong" was itself an unverified claim
   presented as fact - the lesson is to re-check a "this looks wrong"
   conclusion by hand before writing it down, not just to fix the bug.

## What's proven vs. what's still open

**Proven**, by a harness that links the real code (not a mirror of it):
- Message packing/unpacking is correct for 6-char and 5→6-char-padded and
  prefix-digit callsigns, all four grid corners, and power quantization
  including rounding edge cases.
- The convolutional encoder + interleaver + Fano decoder round-trip a
  message through the full 162-symbol channel representation with zero
  errors, and recover it correctly under 1- and 2-symbol errors.
- The decoder does not crash or hang on random noise (20-trial fuzz test,
  10000 cycles/bit budget) — though see the caveat below.

**Explicitly NOT proven yet** (next steps, in rough order):
- ~~Sync-vector interleave against a second independent source~~ — **done**,
  see the correction above and the follow-up session below.
- **False-decode rejection.** Fed pure random noise, the Fano decoder
  "succeeds" nearly every time (a WSPR type-1 message carries no CRC) — a
  real receiver needs a separate quality gate (sync-correlation score
  against the known sync vector, callsign/grid plausibility, or both)
  before trusting a decode. That gate lives with the sync-search work below,
  not in the codec layer tested here.
- **Real soft-decision metrics.** The hard 0/255 two-level table proves the
  decoder logic but is not what a real receiver would build — a proper
  table derived from an actual per-symbol SNR estimate (log-likelihood
  scale) is needed before this can decode a real weak signal, per
  `docs/wspr-phase0-research.md`'s original flag.
- **The DSP front end.** Nothing here touches real audio: deriving a soft
  per-symbol metric from 4-FSK correlation against captured IQ, finding the
  ~1.4648 Hz tone spacing and the transmission's start time within a
  ~110.6 s window, and validating against real WSPR WAV captures. This is
  still, per the scope doc, "probably the majority of the total effort" —
  today's work is the layer underneath it, not a shortcut past it.

## Update — audio-domain decode against a real WAV (same session, continued)

The "next steps" above are now partly done. Added:

- [main/wspr_decode.h](../main/wspr_decode.h) / `.c` — coarse frequency
  candidate detection (one FFT over the whole capture, scored by 4-tone
  comb energy) + per-candidate start-time search + hard-decision tone
  extraction + the three-check plausibility gate (message shape, legal
  power quantization, Fano cycle count).
- [test/wspr_decode_harness.c](../test/wspr_decode_harness.c) — downloads
  are not automated (see below), but the harness runs against
  `test/wav_reference/wspr/150426_0918.wav`, **WSJT's own official WSPR
  sample recording** (sourceforge.net/projects/wsjt/files/samples/WSPR/,
  120 s / 12000 Hz / 16-bit mono), tracked in git alongside the existing
  FT8 reference WAVs.

**Result: 5 of 8 detected candidates decode cleanly** to standard-format US
amateur callsigns (W3HH, WD4LHT, ND6P, W5BIT, KI7CI), each with a legal
WSPR power value and fast Fano convergence (82-102 cycles). The other 3 are
correctly rejected — including, notably, **the file's own strongest signal
by a 3x margin**, which is the interesting result: its huge cycle count
(49400, vs 82-102 for the clean ones) says the decoder had to fight for a
result it shouldn't trust, and the most likely explanation is the one
documented limitation this module doesn't yet handle — frequency drift
over the 110.6 s transmission. Not silently worked around; flagged in
`wspr_decode.h` and asserted in the harness (`3 rejected` is a checked
regression, not just a log line) so a future drift-compensation fix shows
up as "the reject count changed" rather than an unnoticed behavior shift.

There is no independently-published "ground truth" for what this specific
file decodes to — the corroboration is three independent signals agreeing
(real callsign shape, legal power, fast convergence) across 5 different
candidates, not a match against an authoritative answer key. That's real
evidence, not proof; documented as such in the harness's own header.

**Runtime note**: the initial version of the start-time search called
`cos()`/`sin()` inside the innermost sample loop and didn't finish a single
candidate in 2 minutes. Fixed by precomputing twiddle tables once per
candidate frequency (indexed by absolute sample position) so the search
itself is pure array-lookup multiply-adds — full 8-candidate run now takes
~15 s on a laptop.

## Update — interleave cross-check corrected (same session, continued)

Fixed the item flagged throughout this doc as "still open": the interleave
permutation now has a real second-source cross-check against wsprcan's
independent implementation, and it passes. The earlier write-up blamed a
web-fetch transcription error for a mismatch that was actually my own
comparison bug (missing an 8-bit truncation the source code does
explicitly) — corrected in place above rather than silently rewritten, see
item 4 under "Bugs the harness caught".

## Update — frequency drift tested as a hypothesis, NOT confirmed (same session)

Tried a linear drift search (±8 Hz total across the transmission, 0.5 Hz
steps, plus a small start-time nudge) around the rejected strong-signal
candidate (`f0=1500.933 Hz`, `dt=2048`), throwaway tool, not promoted into
`main/`. Result: the best-scoring drift (-1.0 Hz) raised the raw
sync-correlation score by ~21% (199846 → 241820), but the resulting decode
was a **different** implausible callsign (`NE7CCO`), an illegal power value
(54 dBm - not on the legal WSPR set either), and a **worse** cycle count
(256154, vs 49400 at zero drift) - the opposite of what a real fix should
do. A better sync score without a better decode, on a search that also
picked a different message than the baseline, is the signature of chasing
noise, not of correcting a real impairment.

**Conclusion: frequency drift is not confirmed as the explanation** for
this candidate. Could still be drift with a wrong model (linear-centered
may not match a real oscillator's warm-up curve) or too coarse a search,
but could equally be something this simple pipeline was never going to
decode - two overlapping signals near the same frequency, a birdie/local
carrier, or genuinely not a standard WSPR transmission. Not chasing this
further blindly; the honest state is "rejected, cause unconfirmed," and
the existing three-check gate is doing its job either way - it caught a
decode that shouldn't be trusted regardless of why.

## Update — synthetic multi-signal + sensitivity testing (same session, continued)

A second *real* WAV proved to be a dead end: WSJT's own official sample
archive genuinely only has one WSPR file (checked its manifest directly,
`contents_2.5.json` through `contents_3.1.json` - all identical, one
entry). Rather than keep searching indefinitely, pivoted to something
fully controllable: synthesize known-ground-truth WSPR signals (same
precedent as this project's FT8 self-test, which synthesizes real GFSK
audio and decodes it through the real pipeline) and run them through the
real `main/wspr_decode.c` - not a replacement for real-signal testing
(`test/wspr_decode_harness.c` still does that), a complementary check with
exact ground truth. New: `test/wspr_synth_harness.c`.

**Multi-signal separation: PASS.** Three simultaneous synthetic
signals (different callsigns, grids, powers, frequencies within the same
200 Hz sub-band, light AWGN) - all three correctly detected as separate
candidates and decoded correctly. This is real evidence the coarse
frequency-candidate detection genuinely separates overlapping signals,
not just an artifact of the one real file having well-separated ones.

**Sensitivity sweep: informative, and a real methodology bug caught along
the way.** First attempt reported a "dB SNR" figure using a naive wideband
amplitude/noise ratio, and found NO breaking point at all down to -20 dB in
that unit - every test point decoded identically. That result should have
been a red flag on its own (a decoder that never fails as signal shrinks
isn't real), and the actual cause was a miscalibrated unit: each symbol's
soft decision comes from an 8192-sample coherent DFT, which has real
processing gain (~39 dB) a wideband ratio doesn't account for. Fixed by
computing SNR properly in WSJT-X's own standard 2500 Hz reference
bandwidth (the unit published WSPR sensitivity figures use) and widening
the sweep range. Corrected result: **this hard-decision decoder works down
to about -22.7 dB SNR (2500 Hz ref) and fails by -25.2 dB** - roughly 3-5 dB
worse than real wsprd's published ~-28 to -31 dB, which is the right shape
of result: hard-decision decoding is expected to lose a few dB of coding
gain versus soft-decision, not perform identically to it. A number that
came out worse than the reference implementation, by a plausible margin,
for a well-understood reason, is a healthier result than one that looked
suspiciously perfect - which the first (wrong) unit did.

### What's still open

- **Why the strongest real-signal candidate fails** — open question, not
  confirmed to be frequency drift (see above). Worth revisiting with a
  proper wsprd-style 2D (frequency × drift) joint search rather than a 1D
  drift search bolted onto an already-fixed frequency, if picked up again.
- **Real soft-decision metrics** — still hard-decision only. Now has an
  actual measured cost (roughly 3-5 dB of sensitivity vs. published
  wsprd numbers, see above) rather than being an unquantified gap - closing
  it would directly buy back that margin. `ast/wsprd`'s `metric_tables.c`
  (K9AN's real soft-decision table) is a concrete starting point if this
  is picked up.
- **A second real WAV** — not found through the official channel; the
  synthetic multi-signal test above is the practical substitute for now.
- **Device integration (Phase 2)** — deliberately not started. Per the
  scope doc's own rule, real-signal validation was worth more than moving
  to firmware, and this session added two genuinely new pieces of evidence
  (multi-signal separation, a measured sensitivity number) rather than
  just one WAV file's results.
