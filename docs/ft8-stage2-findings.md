# FT8 Stage 2 — Residual Subtraction Weak-Signal Rescue: Findings

**Date:** 2026-07-07
**Scope:** Offline test harness (`test/`), not on-device firmware. No QMX interaction.
**Test file:** `test/wav_reference/websdr_test1.wav` (18 documented decodes), plus 7 further files for generality.

---

## Summary

Stage 2 adds a **residual-subtraction pass** on top of the normal FT8 decoder: after the
baseline decode, every decoded signal is subtracted from a copy of the waterfall, and the
residual is re-searched and re-decoded to recover signals the baseline missed.

**Result: the technique works, but the gain is modest.** On `websdr_test1` it rescues
**1 of the 5** baseline-missed signals (`CQ DX Z33Z KN11`), lifting the file from
**13/18 (72.2%) → 14/18 (77.8%)**. Across 8 files it recovers **+1 to +2 genuine,
ground-truth-verified decodes on 4 of 8 files**, and nothing on the other 4. A `scale = 0.0`
control (identical pipeline, no subtraction) rescues **0** on every file, proving the gain
comes from the subtraction itself and not from any pipeline/heap difference.

The 4 genuinely-weak `websdr_test1` targets (−6 to −24 dB) are **not** recovered by this
magnitude-domain method — those require coherent time-domain subtraction (the known
long-term path).

---

## Correction to the prior "mask removal proven / 700 candidates" claim

The earlier Stage 2 code (commits `0f724ac`…`17262fc`) reported **700 residual candidates
(140 per pass), 100% pass success** and concluded "mask removal proven." **That result was a
heap-corruption artifact, not real unmasking.**

Root cause: the decoder's waterfall (`mon->wf.mag`) is a `uint8_t` array (magnitude stored as
`(dB + 120) × 2`). The old code reinterpreted it as `float*` and `memcpy`'d/subtracted through
that alias. The subtraction wrote float-arithmetic garbage over the first few kilobytes of the
waterfall, and `ftx_find_candidates()` then returned exactly **140 = `MAX_CANDIDATES`** — the
heap-full ceiling — from corrupted sync scores. The fixed "140 per pass, always" is the tell.

The rewrite (this work) operates **directly on the native `uint8_t` waterfall** and does real
magnitude-domain subtraction. With it, candidate counts vary with the data (130–168), the
subtraction is bounds-clean (verified with an instrumented guard + tail canary), and the
rescued decodes are validated against the truth files.

---

## Method

For each baseline-decoded message the harness has: the decoded text, the 79 per-symbol tones
(`ftx_decode_status_t.symbols`, sync positions filled from the Costas pattern), and the
candidate position (`time_offset`, `freq_offset`, `time_sub`, `freq_sub`).

Subtraction (`test/ft8_stage2.c`, `subtract_message`):

1. Copy the real `uint8_t` waterfall into a residual buffer.
2. For each of the 79 symbols, locate the signal's tone bin
   (`freq_offset + tone`) at that time block.
3. Estimate the local noise floor as the mean dB of the 7 **non-signal** tone bins at that
   symbol.
4. **Notch** the signal bin toward the noise floor across all time/frequency OSR sub-cells, in
   the linear-power domain:
   `p_res = p_noise + (1 − scale)·(p_sig − p_noise)` → back to dB → `uint8`.
   `scale = 1.0` notches fully to the noise floor; `scale = 0.0` removes nothing (the control).
5. Re-run `ftx_find_candidates()` + `ftx_decode_candidate()` on the residual, dedupe against the
   baseline (by message hash), and report any new CRC-valid decode.

CRC validity matters: every reported "new decode" passed the 14-bit FT8 CRC, so the
false-positive rate per candidate is < 0.01%. The reported rescues were additionally confirmed
present in each file's truth `.txt`.

---

## Results

### `websdr_test1` — baseline vs Stage 2

| | Decodes | Rate |
|---|---|---|
| Baseline (Stage 1) | 13 / 18 | 72.2% |
| **+ Stage 2 residual subtraction** | **14 / 18** | **77.8%** |

Rescued: `CQ DX Z33Z KN11` (documented −1 dB). The 5 baseline-missed targets were:

| Target | Doc. SNR | Rescued by Stage 2? |
|---|---|---|
| CQ DX Z33Z KN11 | −1 dB | **yes** |
| R2ATW IZ0VLL −16 | −6 dB | no |
| LZ1CWK DC8VA RR73 | −14 dB | no |
| YO7CGS A41ZZ −11 | −15 dB | no |
| CQ EA1HTF IN52 | −24 dB | no |

### Scale-factor tuning (`websdr_test1`)

| Scale | Residual candidates | New decodes | Rescued targets |
|---|---|---|---|
| **0.00 (control)** | 160 | **0** | **0** |
| 0.80 | 132 | 1 | 1 |
| 0.85 | 130 | 1 | 1 |
| 0.90 | 133 | 1 | 1 |
| 0.95 | 134 | 1 | 1 |
| 1.00 | 168 | 1 | 1 |

Reading:
- **Control (0.00) rescues 0** → the rescue is caused by the subtraction, not the pipeline.
- **0.80–0.95 is the sweet spot.** Subtracting the strong signals also removes their *spurious*
  sync candidates, so the candidate count drops (160 → ~132) while the real weak candidate now
  survives and decodes.
- **1.00 over-notches:** flattening cells fully to the noise floor introduces new noise-floor
  artifacts that raise the candidate count back to 168 without yielding any additional decode.
- **Recommended default: `scale = 0.90`** (mid-plateau; matches the prior default and is furthest
  from the 1.00 over-notch edge).

### Generality (8 files, control vs subtraction)

| File | Baseline | New decodes @ 0.00 (control) | New decodes @ 0.90 |
|---|---|---|---|
| websdr_test1 | 13 / 18 | 0 | **1** |
| websdr_test2 | 19 / 21 | 0 | **1** |
| websdr_test3 |  9 / 11 | 0 | 0 |
| websdr_test5 | 17 / 27 | 0 | **2** |
| websdr_test8 | 17 / 26 | 0 | **1** |
| 191111_110200 | 3 / 5 | 0 | 0 |
| 191111_110630 | 10 / 15 | 0 | 0 |
| 191111_110115 | 0 / 1 | – | – |

Every ground-truth-verified example:
`websdr_test5` → `CQ UA3YFS KO73`, `UT8UU ON4FG RR73`; `websdr_test8` → `CQ DM1YS JO30` — all
present in their truth files. The control column is **0 everywhere**.

---

## What Stage 2 does and does not buy

**Does:** recovers real signals that the baseline drops because a stronger signal's energy (and
its many spurious sync candidates) crowds the fixed-size candidate heap or masks the weaker
signal in nearby time/frequency cells. Typically **+1 to +2 decodes per busy slot**, at
essentially no false-positive risk (CRC-gated).

**Does not:** recover deeply-weak signals (roughly < −6 dB here). Magnitude-domain notching on
the coarse (6.25 Hz / 0.16 s) waterfall cannot separate two signals sharing the same cells, and
it discards phase, so it cannot coherently cancel a strong signal to expose a co-located weak
one. The 4 unrescued `websdr_test1` targets (−6…−24 dB) fall in this class.

**Path to more:** coherent **time-domain** subtraction (re-synthesize each decoded signal's
complex waveform — the GFSK synthesizer already exists in `ft8_stage2.c` — align in phase, and
subtract from the audio before re-running the full STFT). This is the established WSJT-X-style
approach and the correct next step if further weak-signal yield is wanted.

---

## Implementation notes / gotchas

- **`ftx_message_decode()` dereferences its `offsets` argument unconditionally** (`message.c`
  line ~514: `offsets->types`). Passing `NULL` segfaults — always pass a real
  `ftx_message_offsets_t`.
- **Do not repeatedly `malloc`/`free` the ~160 KB waterfall buffer per call.** Doing so was
  unstable on the MinGW toolchain here (crash on the 3rd cycle in `free`/`malloc`, with my own
  writes proven bounds-clean by a guard + canary). The residual scratch buffer is now allocated
  once and reused across the scale sweep — robust and faster.
- The residual pipeline uses a 200-slot candidate heap and 20 LDPC iterations; the `scale = 0.0`
  control shares these exactly, so the comparison is apples-to-apples.

## Reproduce

```bash
cd C:\dev\qmx-panadapter\test\build
ninja
./ft8_test_harness.exe ../wav_reference/websdr_test1.wav ../wav_reference/websdr_test1.txt
```

Stage 2 output appears after the SNR-calibration section: the residual rescue summary followed
by the scale-factor tuning sweep (including the `0.00` control row).
