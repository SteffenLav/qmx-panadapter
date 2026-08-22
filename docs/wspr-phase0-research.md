# WSPR Phase 0 — research findings

Companion to `docs/wspr-scope.md`. This is Phase 0 only: no code, no firmware
touched. Each of the three Phase 0 items from the scope doc is answered below
with sources, so Phase 1 (the host decoder harness) can start from verified
constants instead of guessing them.

## 1. CAT tone resolution — RESOLVED, sufficient

QMX CAT manual (`docs/qmx-reference/cat_104.txt`, `TA` command):

> Set: In Digi mode, sets the transmitted audio tone frequency, which may be
> specified to a decimal fraction of a Hz... For example `TA1502.34;` would
> set the audio tone to 1502.34 Hz.

The example carries two decimal places, i.e. 0.01 Hz resolution — comfortably
finer than WSPR's 1.4648 Hz tone spacing needs (a 1 Hz rounding error would
already be a third of a tone; the radio doesn't even round to that coarsely).

Our own FT8 TX code already sends fractional Hz: `ft8_tx.c` formats every
symbol's tone as `TA%.2f;` (`tx_cmd(t0, sim, "TA%.2f;", (double)freq)`). So
the CAT plumbing is already proven at finer-than-needed resolution — nothing
about the radio or our existing `tx_cmd`/`sleep_until` mechanics blocks WSPR
TX. **TX is "wire up what already exists," not "point at Virtual U3S
instead"** — though Virtual U3S remains a valid fallback if the timing
budget (162 symbols at ~682.66 ms instead of 79 at 160 ms) turns out to be
harder to hold than expected.

## 2. WSPR type-1 message + FEC constants — RESOLVED, cross-verified

Pulled from three independently-maintained implementations plus the original
protocol-author mailing list thread, specifically so no single source could
be silently wrong (same discipline as the ARRL FD verification against
`packjt77.f90`):

- `lib/wsprd/wsprd.c` and `lib/wsprd/fano.c`, WSJT-X's own source
  (github.com/saitohirga/WSJT-X, an official auto-mirror of the sourceforge
  tree) — the authoritative decoder.
- `wspr.cpp`, WsprryPi (github.com/JamesP6000/WsprryPi) — an independent
  encoder-side implementation (Raspberry Pi WSPR beacon).
- `wspr.c`, wsprcan (github.com/mike-hb/wsprcan) — an independent decoder
  port.
- wsjt-devel mailing list thread confirming the generator polynomials
  directly from the protocol's own authors (K1JT/K9AN).

All three source trees agree on every constant below.

### Convolutional code

- Rate 1/2, constraint length K=32 (Layland-Lushbaugh polynomials, the set
  WSJT-X actually ships with — `fano.c` also has NASA-standard and
  Massey-Johannesson polynomial sets available behind `#ifdef`, unused).
- Generator polynomials: `POLY1 = 0xF2D05351`, `POLY2 = 0xE4613C47`.
- Message: 50 bits (28-bit callsign + 15-bit grid + 7-bit power) padded to
  81 bits with 31 zero flush bits, encoded to 162 output bits (81 input bits
  × rate 1/2).

### Callsign packing (28 bits, mixed-radix)

From `wspr.cpp`, matching WSJT-X's own scheme — a 6-character
space-padded callsign `c[0..5]` packed as:

```
n1 = (c[0] is digit ? c[0]-'0' : c[0]-'A'+10, or 36 if c[0] is space)
n1 = 36*n1 + (same rule for c[1])
n1 = 10*n1 + (c[2] - '0')                      // must be a digit
n1 = 27*n1 + (c[3]=='A'..'Z' ? c[3]-'A' : 26)  // 26 = space
n1 = 27*n1 + (c[4] rule as above)
n1 = 27*n1 + (c[5] rule as above)
```

i.e. base-36, base-36, base-10, base-27, base-27, base-27 — the same
mixed-radix shape as the callsign field WSJT-X uses elsewhere (recognisable
from `packjt77.f90`, already used for FT8/FD). Only 6-character callsign
forms fit natively; compound calls need the type-3 hashed-callsign path,
out of scope for a first cut per the scope doc.

### Grid + power packing (22 bits: 15-bit grid + 7-bit power)

```
n2 = (grid_number << 7) | (power_dbm + 64 + nadd)
```

where `grid_number` is the 4-character Maidenhead locator (first two pairs
only — WSPR type-1 carries a 4-char grid, not 6) encoded 0..32399 by the
standard `((179 - 10*(g0-'A') - (g2-'0')) * 180) + (10*(g1-'A') + (g3-'0'))`
shape (need to re-derive/verify the exact formula against `wsprd_utils.c`'s
`unpackgrid()` in Phase 1 — this doc only confirms the *bit width and
position*, not yet the exact inverse formula), and `nadd` flags whether this
transmission carries a nonstandard callsign follow-up (0 for a plain type-1
report).

### Interleaving (161-bit convolutional output → 162 channel symbols)

Bit-reversal permutation over an 8-bit index, skipping any reversed index
≥162 (162 = 2^8 - 94, so roughly a third of the 256 reversed-index slots are
discarded):

```c
for (i = 0; i != 162; i++) {
    // reverse the low 8 bits of a running counter k, keep only
    // reversed values < 162, and use the i-th such value as the
    // interleaved bit's source position
}
symbols[j] = npr3[j] | (encoded_bit[i] << 1);
```

`wsprcan`'s `deinterleave()` gives the inverse (decode-side) form of the same
bit-reversal as a closed-form magic-number trick:
`j = ((i * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL >> 32` — worth
using directly in the decoder rather than re-deriving the encode-side loop
and inverting it by hand.

### Sync vector (162 bits) — confirmed byte-identical across all 3 sources

```
1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,
0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,
0,0,0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,
1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,
0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,
0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,
0,1,0,0,0,1,1,1,0,0,0,0,0,1,0,1,0,0,1,1,
0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,
0,0
```

Each transmitted symbol's 2-bit value is `sync_bit | (data_bit << 1)`, giving
the 4-FSK tone index (0-3) directly.

## 3. Fano decoder — RESOLVED, confirmed as the right choice, entry point identified

`fano.c` (WSJT-X's own, K9AN/K1JT authorship) is the reference. Confirms the
scope doc's assumption: this is a genuine sequential (Fano) decoder, not
Viterbi — infeasible at K=32 (2^31 states) as already noted. Key shape for
Phase 1 to target:

```c
int fano(unsigned int *metric, unsigned int *cycles, unsigned int *maxnp,
         unsigned char *data, unsigned char *symbols, unsigned int nbits,
         int mettab[2][256], int delta, unsigned int maxcycles);
```

- `mettab[2][256]` — a precomputed soft-decision metric table (indexed by
  [transmitted bit][received symbol byte 0-255]); generating this table
  correctly from the channel SNR estimate is itself a real piece of Phase 1
  work, not a constant to just copy.
- `delta` — threshold step size for the Fano search.
- `maxcycles` — bounds the search; the decoder returns failure rather than
  hanging on a bad candidate, which matters for a bounded per-slot decode
  budget on this device (same concern as FT8's `FT8_DECODE_BUDGET_MS`).
- Depends on an `ENCODE(sym, encstate)` macro (in `fano.h`) that re-encodes
  candidate bit sequences during the tree search using the same POLY1/POLY2
  taps above — so the encoder and decoder share the generator-polynomial
  constant, same as ft8_lib's own encode/decode symmetry.

## What Phase 1 should pull next (not done here — this is research, not code)

- `wsprd_utils.c`'s `unpackgrid()`/`unpackcall()`/`unpack50()` for the exact
  inverse-packing formulas (this doc confirms bit widths and the general
  packing shape, not yet byte-verified formulas — do that verification
  *in* the host harness, the same way `test/ft8_cq_encode_harness.c` checks
  itself against known-good vectors, not by trusting this summary).
- `fano.c`'s full body and `fano.h`'s `ENCODE` macro, to port both the
  metric-table generator and the search loop.
- A small set of known-good WSPR WAV captures + their expected decoded
  messages, to use as the harness's ground truth (per the scope doc's
  Phase 1 plan) — public WSPR WAV archives exist for this purpose; picking
  specific ones is a Phase 1 task.

## Conclusion / recommended next step

All three Phase 0 questions are answered and none of them block the project:

1. CAT resolution is sufficient — confirmed from the manual and from our own
   already-shipping `%.2f` TA formatting.
2. The message-packing and sync-vector constants are pulled from source and
   agree across three independently-maintained trees — safe to build Phase 1
   against them, with the one flagged exception (exact grid-locator inverse
   formula) to nail down with a harness self-test rather than by hand.
3. Fano is confirmed correct and its reference entry point is identified.

**Recommended next step: start Phase 1** — `main/wspr_decode.c` +
`test/wspr_decode_harness.c`, portable/no-ESP-deps, built against a real
captured WSPR WAV file, following the existing harness convention.
