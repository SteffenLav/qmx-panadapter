# WSPR Phase 1 — status

Companion to `docs/wspr-scope.md` and `docs/wspr-phase0-research.md`. This is
the protocol-layer slice of Phase 1: message packing, the K=32 convolutional
code, interleaving, and the Fano decoder — proven on the PC, no ESP deps, no
firmware touched yet.

## ⚠ Licensing incident and fix (2026-08-22) — read this first

Everything below this section describes work that happened in TWO passes:
an initial implementation that turned out to have a real licensing problem,
and a same-day clean-room rewrite that fixed it. Recording both, not just
the fixed end state, because the mistake and how it was caught are worth
keeping.

**What happened.** This repo is MIT-licensed. The initial WSPR protocol/FEC
implementation was built by reading (and in places directly porting)
several WSJT-X-family GitHub projects: WSJT-X's own `lib/wsprd/fano.c` and
`wsprd_utils.c`, plus `robertostling/wspr-tools` and `mike-hb/wsprcan` for
cross-checks and pack-side formulas. All four of those are **GPL v3**
(confirmed via each repo's GitHub license API, not assumed). A fifth
source, `JamesP6000/WsprryPi`, shows as **"Other/NOASSERTION"** - GitHub
hosting a repo does not itself grant a license, so an unclear license
grants no rights either. None of that was safe to fold into an MIT
project: `main/wspr_fano.c`'s Fano decoder was described in its own commit
message as "ported near-verbatim" from `fano.c`, and `main/wspr_proto.c`'s
unpack functions were "ported byte-identical" from `wsprd_utils.c` - both
literal admissions of copying GPL source structure, not just facts about
the protocol.

**How it was caught.** Continuing Phase 1 work (better soft-decision
metrics, understanding a real-signal decode failure), the natural next
step was K9AN's `metric_tables.c` - and before using it, checking where it
actually lived turned up the `wsprd.c` file right next to it carrying an
explicit `License: GNU GPL v3` header. That prompted checking this repo's
own `LICENSE` (MIT) and then, once the mismatch was obvious, going back
and checking every WSPR source used so far - which is how the already-
committed `fano.c` port and the pack-side sources turned out to be the
same problem, not just the one file about to be added.

**The fix — clean-room rewrite of everything sourced from those repos.**
- `main/wspr_fano.c`'s Fano decoder: rewritten from the algorithm's
  published 1963 rules (Fano's original paper predates WSJT-X by decades;
  independent descriptions exist in the Wikipedia "Sequential decoding"
  article and standard coding-theory course notes) using this module's own
  data layout (parallel `gamma[]`/`enc_state[]`/`stage[]` arrays with an
  explicit backtrack cascade) instead of Karn's `struct node` array-of-
  precomputed-branch-metrics design.
- `main/wspr_proto.c`'s callsign/grid/power packing: rewritten from the
  WSPR message SPECIFICATION (the standard-callsign template, the
  Maidenhead grid system - protocol/geographic facts, not copyrightable
  code) with pack and unpack sharing ONE symmetric bit-packing design
  instead of being two separately-authored halves the way the ported
  version was.
- `test/wspr_codec_harness.c`'s interleave cross-check: the "magic number"
  one-liner copied from `wsprcan` (itself GPL) was replaced with an
  independent, obviously-correct-by-inspection naive bit-reversal - it
  didn't even need to be sourced from anywhere, since 8-bit bit-reversal is
  a generic, widely-published technique, not a WSPR-specific fact.
- **What was kept, deliberately:** the 162-bit sync vector, the generator
  polynomials (`0xF2D05351`/`0xE4613C47`), and the general bit-width
  layout (28-bit call / 15-bit grid / 7-bit power / 31-bit flush). These
  are DATA - facts about how the WSPR protocol is defined, not anyone's
  creative expression - and copyright doesn't protect facts. Reading them
  from a GPL source to confirm they're correct is fine; copying that
  source's CODE is what wasn't.

**The rewrite caught two genuine bugs of its own**, both fixed and now
covered by the test suite:
1. The Fano decoder's threshold-loosening logic initially jumped back to
   the root on every loosening event instead of resuming at whatever depth
   got stuck - this passed single-symbol-error correction (162/162) but
   silently broke two-symbol-error correction (94/162, should be 162/162).
   Fixed by loosening in place, matching the algorithm's actual
   requirement (not just Karn's specific implementation of it).
2. The callsign unpacker's range-check used the wrong radix for the
   leading (most-significant) character - 36 instead of the correct 37
   (alnum values 0-35 plus 36 for "absent") - which rejected every
   valid callsign whose digit sits at position 1 (K1ABC, W1AW, G0UPL) while
   accepting ones at position 2 (OZ1LAV, VE3XYZ, 4X1XX) by coincidence.
   Caught immediately by the round-trip test.

**Verification that the rewrite is behaviorally correct, not just
differently-licensed:** all three existing test harnesses
(`wspr_codec_harness`, `wspr_decode_harness`, `wspr_synth_harness`) were
rerun after the rewrite and produce **identical results** to before -
same 11/11 codec round-trip cases, same 162/162 single- and two-symbol
error correction, and critically, **the same 5 real callsigns and
legitimate grid squares decoded from WSJT's real reference WAV** (W3HH/
EL89/30, WD4LHT/EL89/30, ND6P/DM04/30, W5BIT/EL09/17, KI7CI/DM09/37) and
the same 3 candidates correctly rejected. That real-world decode match is
the strongest evidence the rewrite's independently-derived bit-layout
formulas are actually correct, not just internally self-consistent - a
formula that only agreed with itself could still be wrong in a way that
happened to cancel out; one that keeps decoding real over-the-air signals
correctly has been checked against reality.

## What exists

- [main/wspr_proto.h](../main/wspr_proto.h) / `.c` — WSPR type-1 message
  pack/unpack (callsign + 4-char grid + power dBm <-> the 50-bit message).
  Clean-room implementation (see the licensing section above): pack and
  unpack share one symmetric bit-packing design, derived from the protocol
  specification rather than ported from any single source.
- [main/wspr_fano.h](../main/wspr_fano.h) / `.c` — the convolutional
  encoder, the interleave/deinterleave permutation, sync-vector combine,
  and the Fano sequential decoder. Clean-room implementation of Fano's
  published algorithm (see the licensing section above).
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

## Update — soft-decision metric attempted, NEGATIVE result, not shipped

Followed up on "real soft-decision metrics" (previously flagged as having
a measured ~3-5 dB cost vs. published wsprd numbers) with a from-scratch
Monte Carlo simulation: `test/wspr_metric_sim.c` simulates this decoder's
own channel statistic (the coherent-DFT tone-power difference D each
symbol reduces to) under AWGN at a chosen amplitude, and builds an
empirical log-likelihood-ratio metric table from the resulting
histograms - fully original, not derived from anyone's published table
(this was in fact what prompted checking K9AN's `metric_tables.c`'s
license in the first place, which is what surfaced the licensing problem
recorded near the top of this document).

**Result: negative, table not shipped.** A single fixed-scale table,
calibrated at one amplitude, decodes correctly only in a narrow band
around that calibration point and fails outside it - including at
STRONG signal (+6.8 dB SNR, where the existing crude hard-decision table
works trivially). Confirmed with two different scales and up to 250,000
trials each; not a statistics problem, a structural one. D scales
roughly with amplitude² (it's a power difference), so a signal several
times stronger than the calibration point saturates nearly every symbol
to byte 0 or 255, and the noisy low-sample-count table entries at those
extreme bins (visibly single-digit counts in the printed tables) get
trusted as if reliable. This is exactly why wsprd itself doesn't use one
fixed table - K9AN's `metric_tables.c` ships FOUR tables for different
Es/No points and selects between them.

The existing 2-level hard-decision table stays the shipped default: no
calibration fragility, and already measured at -22.7 dB sensitivity,
which this attempt did not beat. Real follow-up work, not done here:
either multiple tables selected by an estimated operating SNR (matching
wsprd's own approach, but built fresh rather than copied), or per-capture
normalization - deriving the quantization scale from the ACTUAL candidate
signal's own measured |D| distribution rather than a pre-baked constant.
`test/wspr_metric_sim.c` is kept as the tool to pick this up with, not
deleted, since the simulation methodology itself worked fine - only the
fixed-scale design choice didn't.

## Update — per-capture normalization: works great synthetically, REGRESSES real WAV, reverted

Picked up the "per-capture normalization" follow-up from above:
`test/wspr_metric_sim.c` rewritten to train a pooled table across 8
amplitudes (200,000 total trials), each batch normalized by its own
mean(|D|) before quantizing - and the self-check applies the identical
per-capture normalization at decode time (estimating scale from the
candidate's own 162 symbols, exactly as a real decode would).

**Synthetic result: dramatic.** Single-message sweep decoded correctly
from +10.3 dB down to -24.2 dB, first failure at -26.4 dB - about 2-3 dB
better than the -22.7 dB hard-decision baseline. Wired into
`main/wspr_decode.c` (`wspr_build_soft_metric_table()` +
`wspr_deinterleave_scores()`, new function, added to `wspr_fano.c`/`.h`
for carrying a soft score instead of a hard bit through the interleave
permutation).

**Real-world result: regression, reverted the same session.** Run against
the actual reference WAV, plausible decodes dropped from 5/8 to 1/8 - most
candidates that used to decode cleanly now either time out or converge on
garbage. Reverted `wspr_decode_candidate()` back to the hard-decision
table immediately; verified all three test suites (codec, real-WAV,
synthetic) pass again at the known-good baseline before moving on.

**Why this matters more than the numbers:** this is the textbook
"validated in simulation, fails in reality" trap, and it happened despite
a careful, methodical simulation approach. The likely cause: the
synthetic channel model is one or a few clean tones plus i.i.d. Gaussian
noise, and real captured audio apparently has enough else going on (other
in-band signals, non-ideal noise characteristics, whatever a 120-second
real HF recording actually contains) that a table tightly calibrated to
the clean synthetic statistic isn't robust to the mismatch - while the
simple hard-decision table, needing no calibration at all, doesn't have
that fragility.

**The standing rule this reinforces**: a synthetic sensitivity sweep
passing is evidence, not proof. The real WAV test is what actually
decides whether a decoder change is a genuine improvement, and it must be
run - and trusted over synthetic results when they disagree - before
anything is called a win. `wspr_build_soft_metric_table()` and
`test/wspr_metric_sim.c` are kept (real, working infrastructure, and the
synthetic-only validation gap is now a known, documented risk rather than
an invisible one) but NOT used by default. If picked up again: test
against the real WAV at every iteration, not just at the end, and
consider whether the synthetic channel model needs to include something
closer to real band conditions (multiple signals, real noise) rather than
assuming AWGN-only is representative.

### What's still open

- **Why the strongest real-signal candidate fails** — open question, not
  confirmed to be frequency drift (see above). Worth revisiting with a
  proper wsprd-style 2D (frequency × drift) joint search rather than a 1D
  drift search bolted onto an already-fixed frequency, if picked up again.
- **Real soft-decision metrics** — still hard-decision only, by choice.
  TWO from-scratch attempts now (see the two updates above): a fixed-scale
  table (failed - too narrow a calibration band) and a per-capture-
  normalized pooled table (worked great synthetically, REGRESSED the real
  WAV 5/8 → 1/8, reverted). The open question isn't "how to build the
  table" any more, it's "why does a synthetic AWGN-only channel model not
  predict real-world behavior" - that needs investigating BEFORE a third
  attempt, or it'll likely repeat the same trap. Do NOT reach for
  `ast/wsprd`'s `metric_tables.c` even for "inspiration on the numbers",
  it's GPL v3, same problem this document's licensing section is about.
- **A second real WAV** — not found through the official channel; the
  synthetic multi-signal test above is the practical substitute for now.
- **Device integration (Phase 2)** — deliberately not started. Per the
  scope doc's own rule, real-signal validation was worth more than moving
  to firmware, and this session added two genuinely new pieces of evidence
  (multi-signal separation, a measured sensitivity number) rather than
  just one WAV file's results.
