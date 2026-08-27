# WSPR RX sensitivity: closing the gap to wsprd

Phase 1 proved the decoder worked. Phase 2 made it fit on the device. This is
Phase 3: making it *hear* as well as the reference decoder, measured rather
than asserted.

## The measurement

Four reference recordings, all 120 s / 12 kHz / mono, all also decoded by
WSJT-X's own `wsprd.exe` on **byte-identical audio**:

| file | what it is |
|---|---|
| `150426_0918.wav` | WSJT's official WSPR sample recording |
| `260824_1906/1910/1914.wav` | captured off this station's own antenna, 20 m |

`wsprd` finds **41 decodes** across the four. Four of those are `<...>` rows -
hashed nonstandard callsigns, a message type this decoder does not implement at
all - so the reachable target is **37**.

The unit throughout is **unique confirmed stations**: a decode counts only if
`wsprd` found the same callsign in the same file. Counting our own decodes is
not a sensitivity measurement, because under noise this decoder fabricates.
That trap is recorded at the top of `tools/wspr_noise_ladder.py` and it is why
every number below is scored against `wsprd`'s list.

## Where it went

| state | unique confirmed | fabrications |
|---|---|---|
| before this work | 17 | 0 |
| + soft-decision metric | 21 | 0 |
| + re-encode agreement check | 21 | 0 |
| + fine frequency search | 21 | 0 |
| + sidelobe-only candidate suppression | **23** | **0** |

At 20 candidates - what the device actually runs - the figure is the same 23,
so the extra candidate slots are not where the remaining gap lives.

## The four changes, and why each one was needed

### 1. Soft decisions, with a table fitted to our own normalisation

The Fano search was being fed **hard bits**: a symbol that barely favoured 1
was declared exactly as trustworthy as one that screamed it. For a K=32
rate-1/2 convolutional code that throws away roughly 2 dB.

A soft metric had been tried before and **regressed the real WAV badly** - 1 of
8 plausible decodes instead of 5 (`docs/wspr-phase1-status.md`). Two things
were different this time, and both matter:

- **Amplitude, not power.** Squaring hands the sum to whichever few symbols
  happened to be loudest, which under fading is precisely the wrong emphasis.
- **The table and the normalisation were fitted together.** A metric table *is*
  the statistics of a normalised soft symbol; if the decoder normalises
  differently, the table describes a distribution that never arrives.
  `tools/gen_wspr_metric.py` simulates whole 162-symbol transmissions and
  normalises each one exactly the way `try_soft_decision()` does.

The table is **generated, not copied**. WSJT-X ships an equivalent one under
GPL and this project is MIT, so vendoring it was never an option - but it also
did not need to be, because the table is simulated statistics rather than a
magic constant. Generating it is what allowed it to be fitted to our own
normalisation, which is the part that had failed before.

> **DELTA IS IN THE SAME UNITS AS THE METRIC.** The first version passed
> `delta = 2` - correct for the old +1/-3 hard table - against a table scaled
> by 1000. Every hard candidate then ran to the 1,620,001-cycle ceiling and
> gave up, and the whole soft path measured as **worth exactly nothing** (17
> decodes, unchanged). `wsprd` uses 60 against metrics scaled by 10, i.e. six
> bits; ours is the same figure in our units. A wrong scale here does not look
> like a tuning problem. It looks like a decoder that cannot hear.

### 2. The re-encode agreement check - the only test that consults the audio

**WSPR carries no checksum.** A wrong-but-valid codeword is undetectable from
the message alone, which is why no callsign, grid or power heuristic can ever
catch one: `PD2WND EL53 13 dBm` was reported three times at f=1473.08 Hz,
exactly where the real `PD2LEO` lives. Not noise inventing a station - a
near-miss decode of a real one.

`score_agreement()` re-encodes the decoded message back to its 162 channel
symbols and asks whether the received audio actually said that. Every other
check in `accept_if_plausible()` tests the message against itself.

Measured across the four files and three search settings - nine fabrications
against 21 confirmed decodes, no overlap:

| | agree_soft |
|---|---|
| fabrications (9) | 0.355 - 0.513 |
| confirmed decodes (21) | 0.655 - 0.914 |

`WSPR_AGREE_MIN` sits at **0.58**, in the middle of the gap rather than tuned
to either edge - the same way the power and cycles guards were set.

> **THIS IS WHAT MADE THE REST OF THE WORK POSSIBLE.** With no way to tell a
> good answer from a bad one, widening the frequency search from 0.0 to
> 0.7 Hz *gained* four decodes and *lost three real ones* - PA4JAM, PE1JXI and
> G8ORM, each pre-empted by a wrong codeword found at a wrong frequency - and
> paid three fabrications for the privilege. The search was never the problem.
> Once a bad answer can be recognised, every hypothesis can be tried and
> scored and the best-agreeing one kept, so a wrong-frequency decode simply
> loses to the right one instead of arriving in its place.

### 3. Fine frequency search

The candidate frequency comes from an averaged periodogram over the whole
120 s window, so it is the frequency of the strongest **bin**, not of any one
station. `sync_score()` had to be fixed first, in two ways, before it could
refine anything:

- **It was unnormalised**, so the search that picks the start time was partly
  maximising total energy - a strong neighbour sliding into the window scores
  well without being in sync at all. Dividing by the total makes it a
  correlation in [-1, 1], which is also what makes it comparable across
  frequency trials and what makes an absolute cost threshold meaningful.
- **It summed powers**, with the same fading problem as the metric above.

`NM7J` is the result worth recording. It is the strongest signal in
`150426_0918.wav` at -1 dB, it never decoded, and
`docs/wspr-phase1-status.md` attributed that to in-transmission fading after a
careful diagnosis. It was a **frequency error**: the periodogram peak sat about
0.2 Hz off and nothing ever corrected it. It decodes now, agreement 0.877.

### 4. Candidate suppression that notches sidelobes instead of blanking bands

`wspr_find_candidates()` blanked **4 tone spacings (5.9 Hz) either side** of
every peak it reported. That made whole stations not merely unranked but
*unreachable* - a frequency that is never a candidate is never tried.

Measured on the 19:10 window, where `wsprd` finds 14 stations:

| stations | separation | shared candidate | decoded |
|---|---|---|---|
| G4FBA / PD2LEO / PA5CA | 1471.8 / 1472.8 / 1474.8 Hz | one, at 1473.08 | 1 of 3 |
| DK8AF / DD3MS | 1521.8 / 1525.8 Hz | one, at 1524.63 | **0 of 2** |

Handed their own frequencies, DK8AF and DD3MS both decode cleanly (agreement
0.757 and 0.835). The blanking was not arbitrary - the comb score genuinely has
sidelobes, because sliding a four-tooth comb by k tone spacings still lands
4-|k| teeth on a real signal - but those are **spikes at known offsets, not a
12 Hz plateau**. Notching the spikes and leaving the gaps between them is worth
two stations on its own, and it is the largest single change of the four.

## The guards that had to be stood down

Two of the four false-decode guards now cost more than they save, and both were
re-measured rather than assumed:

| guard | what it would do to the current decoder |
|---|---|
| SLOW (1000 Fano cycles) | rejects **3 confirmed decodes** - PA4JAM (1716), PA2PGU (1491), DK8AF (1423) |
| NEAR (10 Hz) | rejects **2 confirmed decodes** - 2E0DLC/OE5OSP 5.8 Hz apart, OE5MMP/DK8AF 7.8 Hz apart |
| callsign shape | no effect on any measured data - kept |
| power > 43 dBm | no effect on any measured data - kept |

Neither was wrong when it was set. SLOW is the exact failure its own comment
predicted: **a Fano cycle count is a property of the decode path**, the
soft-decision metric is a different path, and the 16-hour field measurement
that set 1000 was taken on the old one. NEAR's premise - "two WSPR signals
closer than about 6 Hz overlap anyway" - was true when a cluster produced one
candidate, and stopped being true when the candidate finder started resolving
them.

Both remain **measured on every decode**, which is the property the original
design rightly insisted on, so the evidence to bring one back is in any
ordinary session log rather than needing two flashes and two band conditions.

## On the noise ladder

`tools/wspr_noise_ladder.py` raises a recording's noise floor in known steps
and compares both decoders on byte-identical audio, so the horizontal distance
between the curves is the deficit in dB. Same file, same seed, before and
after:

| added noise | wsprd | ours before | ours after | fabrications before | after |
|---|---|---|---|---|---|
| 0 dB | 14 | 5 | **6** | 0 | 0 |
| 1 dB | 8 | 2 | **4** | 0 | 0 |
| 2 dB | 6 | 1 | **2** | 1 | 0 |
| 3 dB | 5 | 0 | 0 | 1 | 0 |
| 4 dB | 4 | 0 | 0 | 2 | 0 |

**Deficit approximately 3 dB before, approximately 2 dB after, and the four
fabrications that used to survive all four guards no longer survive one.**

The curve is also less steep, which was the other half of the old complaint: a
decoder sitting exactly at threshold loses half its yield to one dB of noise.

## What is still missing, honestly

23 of a reachable 37. The remaining 14 split three ways:

1. **Weak** - most sit at -20 to -29 dB in `wsprd`'s own SNR column. Every one
   of them **has** a candidate within about 1 Hz, so this is decoder
   sensitivity, not detection.
2. **Drift** - `wsprd` searches 4 Hz of linear drift either way and we search
   none. Only three rows across the four files have non-zero drift, so this is
   worth about three stations, not more.
3. **Nonstandard callsigns** - the four `<...>` rows are message types this
   decoder does not implement.

Two things were tried and **measured as worth nothing**, recorded here so they
are not repeated:

- **More than one frequency hypothesis.** Trying the 2nd and 3rd sync peaks
  costs 25-45 % more time and found *not one* additional station. The second
  station of a cluster is reached from its own candidate instead, once the
  suppression stops erasing it. The mechanism is kept at `WSPR_HYPOTHESES = 1`
  because it is what gives a rejected answer somewhere to fall back to.
- **More than 20 candidates.** 40 and 60 give the same 23; 80 gives fewer.

## Cost, which is not optional here

The device already skipped **6-7 of its 20 candidates every cycle** before any
of this - 565 `BUDGET CUT` warnings in one capture - so it has no headroom to
spend. Host timing puts the new decoder at roughly **1.35-1.45x** the old one.

`WSPR_MIN_SYNC` (0.075) exists for that reason: it drops a candidate before the
frequency curve and up to nine Fano searches are spent on what is, in a 300 Hz
window, mostly noise floor. It is set **below the weakest real decode observed
(0.0815)** - it is a cost gate and must never be the reason a station is
missed. `wsprd` gates the same quantity at 0.10.

> ⚠ **This gate was written, measured, and then LOST** - reverted along with a
> failed experiment in the same file - while this document and a commit message
> both went on describing it. One flash therefore ran without it, and a device
> log reading `cycles=0 rejected` was read as "the gate did its job" when the
> candidate had in fact run the whole search and up to nine Fano attempts. The
> conclusion drawn from that reading ("the fixed cost is everything") was
> unfounded. Check that a thing is in the file before reasoning about what it
> did, and prefer a measurement the device emits over an inference about which
> code ran.

That still leaves the fixed cost - `mix_decimate()` plus the coarse start-time
search - dominating, and that is the next thing to attack. It is untouched by
this work, and it was already the limiting factor.

> Every threshold here is a property of the DEMODULATOR, not of the signal.
> CLAUDE.md already says this about the Fano cycles guard, after a front-end
> change moved PA3BCA from 336 to 823 cycles and would have rejected a
> confirmed decode. It applies identically to `WSPR_AGREE_MIN` and
> `WSPR_MIN_SYNC`. **Re-measure all three against `test/wav_reference/wspr/`
> after any change to the front end.**

## Not yet verified on hardware

Everything above is measured on the host against recorded audio. It has not run
on the Tab5. The decoder is the same code either way - `test/wspr_cap_sweep.c`
links the real `main/wspr_decode.c` - but the timing is not, and timing is what
the device is short of.

## Reproducing

Reference decoder, for the ground-truth list:

    for f in test/wav_reference/wspr/*.wav; do wsprd -f 14.0956 $f; done

Ours, linking the real decoder (build line also in `test/wspr_cap_sweep.c`):

    gcc -O2 -I main -I components/ft8_lib -o wspr_cap_sweep test/wspr_cap_sweep.c main/wspr_proto.c main/wspr_fano.c main/wspr_decode.c main/wspr_subtract.c components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c -lm
    ./wspr_cap_sweep test/wav_reference/wspr/260824_1910.wav 20

Regenerating the metric table:

    python tools/gen_wspr_metric.py > main/wspr_metric_table.h
