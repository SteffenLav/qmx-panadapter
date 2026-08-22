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
4. **A web-fetched "cross-check" constant was wrong.** I pulled a
   closed-form bit-reversal formula from wsprcan's source via a web fetch to
   cross-verify the interleave permutation, and it doesn't reverse an 8-bit
   value correctly (hand-verified: `reverse8(1)` should be 128, the fetched
   formula gave a nonsense multi-billion-value result for the same input) —
   almost certainly a transcription error from the fetch/summarization step,
   not a real algorithmic disagreement. Removed from the harness rather than
   assert something false; replaced with a bijection check (every one of the
   162 interleave positions is hit exactly once), which is a real property
   test even without a second independent source. **The interleave
   permutation is therefore internally self-consistent and uses the
   documented textbook construction, but has not yet been cross-checked
   against a second independently-sourced implementation** — flagged here
   rather than glossed over.

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
- **Sync-vector interleave against a second independent source.** The one
  cross-check attempted this session used a bad transcription; a real
  second source (ideally by reading actual code rather than a web-fetched
  summary of it) is still worth doing before trusting this against a real
  captured signal.
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

## Recommended next session

Pick up with real captured WSPR audio: get a public WSPR WAV archive
sample + its known-correct decoded message, and start on the sync/frequency
search needed to turn raw IQ into the 162 soft symbol values this session's
decoder already knows how to consume.
