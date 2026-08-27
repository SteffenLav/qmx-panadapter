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


## Cost, measured on the device (2026-08-27)

The host cannot measure this board - hardware double FPU, fast RAM - so all of
this comes from the per-candidate phase breakdown the firmware now logs:

    [mix 770 + coarse 1530 + curve 480 + dec 200 ms]

| | before | after |
|---|---|---|
| per candidate | 11.7 s | ~2.3 s gated, ~3 s decoded |
| candidates skipped per cycle | 6-7 of 20 | 0 |

**Every `double` on a path that runs 100,000+ times per candidate was the whole
problem.** The ESP32-P4's FPU is single-precision, so each one is a
software-library call. Three places: the tone-power array, `sync_score`'s 648
square roots per call, and the local oscillator advancing once per input sample
across all 1.44 M of them. The oscillator is float but **re-seeded exactly**
every 1024 samples rather than renormalised - float alone is not safe, because
a rotation applied 1.44 M times accumulates PHASE error and a renormalise only
fixes MAGNITUDE. That makes it more accurate than what it replaced.

### Two traps, both walked into after writing down the rule

- **`wspr_subtract` called `cos()` and `sin()` per sample** - 5.3 M software
  double trig calls, **67 seconds per subtracted signal**. A cycle went 58 s to
  154 s and skipped 17 of 20 candidates, one commit after this document warned
  about exactly this. Every host test passed. Fixed with the same re-seeded
  float oscillator; the station list is byte-identical.
- **The second pass re-scanned the whole band.** Pass 1 used 82 s of the 115 s
  budget, pass 2 restarted at candidate 0 and was cut after two. Subtraction
  can only reveal something NEAR a signal it removed - everywhere else the
  audio is bit-identical, so re-decoding is guaranteed to reach the same answer
  at full price. A later pass now looks only within 15 Hz of a subtracted
  signal.

## SNR and drift: the two empty columns

Both printed `--` because nothing had measured them, which was correct - a WSPR
spot is a reception report and this project does not publish invented numbers.

**SNR**, validated against wsprd over 23 stations: median 1 dB low, stdev
2.3 dB. No calibration constant; fitting one to 23 points would be fitting to
our own reference set.

> ⛔ **The obvious method is wrong and hides its own error.** Treating each
> symbol's three WRONG tones as free noise samples reads 2-4 dB low on weak
> signals and **23 dB low on the strongest** - and the tell is that the error
> GROWS WITH SIGNAL STRENGTH, which no noise measurement should do. WSPR is
> continuous-phase FSK, so every symbol transition sweeps real signal energy
> into the other three bins; for a strong signal that IS the measurement, and
> the ratio saturates. Noise is now sampled at 14 offsets clear of the
> transmission, combined with the **30th percentile, not the median** -
> contamination is one-sided, and on a crowded band a median is already biased
> (worth 19 dB on KI7CI alone).

**Drift**, from the transmission's own two halves: if frequency moves linearly
by d Hz, the first half sits at -d/4 and the second at +d/4, so d = 2(f2 - f1).
No search needed, because after a decode the transmitted tones are known.

> ⚠ **Weakly validated, and it should stay labelled that way.** All 23 stations
> agree with wsprd within 1 Hz - but 22 of those are wsprd ZEROS, and only one
> non-zero case exists in common (PE1JXI +1, exact). What is established is
> that it does not invent drift and that its noise floor is about 1 Hz. That it
> reads a LARGE drift correctly is not shown.


## A decode costs 35 % more on some boots, and I got the reason wrong twice

`mix_decimate` does a FIXED amount of work every call, yet its mean is 792 ms
on one boot and 1054-1071 on others. Per boot, and constant within one:

| boot | build | mix mean | samples |
|---|---|---|---|
| 106 | v1.9.3-63 (SNR) | 1059 ms | 0 fast / 50 slow |
| 107 | v1.9.3-65 (targeted pass 2) | 1054 ms | 0 / 195 |
| 108 | v1.9.3-68 (UI fixes) | **792 ms** | 165 / 0 |
| 109 | v1.9.6-68 (merged) | 1071 ms | 0 / 75 |

`3ff7e6d` is fast while both the build before it and the merge OF it are slow,
so this does not track the code.

⛔ **Two explanations died on the way, and the second is the lesson.**

1. *"The decode task is the lowest priority in the system, so its wall time is
   load-dependent."* Falsified: correlation with how busy a cycle is comes to
   **0.14**, and the state does not change within a boot.
2. *"`RX<- TM;` appears 7 times in fast windows and 544 in slow ones - the GPS
   second-tick poll is hammering the CAT pipe."* This was about to be reported
   as the cause. It is an **artefact of pooling nine boots**: the log segment
   being analysed contained nine undetected reboots, because a reset does not
   always print `Loaded app from partition` - the marker CLAUDE.md prescribes
   for counting them. **A backward jump in the uptime column is the robust
   test**, and it is what unpicked this.

What remains is where the buffers land: 8.6 MB of PSRAM claimed at page entry,
addresses depending on the heap's state at that moment, and `mix_decimate` is
dominated by streaming through them. Cache-line alignment is the obvious
suspect - so the addresses are now **logged, not acted on**. A few boots settle
it: if fast boots share an alignment slow ones do not, it is confirmed.

⚠ Until then, treat any per-candidate timing as **boot-specific**. Comparing a
number from one boot against another measures the boot, not the change.

## Not yet verified on hardware

The DECODE QUALITY figures above are host measurements against recorded audio.
The COST figures are device measurements. That split is deliberate and it is
the lesson of the session: the host is the only place decode quality can be
compared against wsprd on identical audio, and the device is the only place
timing means anything at all.

Confirmed on the air: the soft path, the agreement check (0.74-0.91 on live
decodes, against a 0.58 threshold), the frequency search, SNR, and 20 of 20
candidates inside budget. NOT yet seen firing on the air: the targeted second
pass, and a large drift.

## The cost pass of 2026-08-27 afternoon: 2.3x less work, and one more station

Four changes, none of them to the decoding algorithm. Measured in
**full-rate-equivalent correlations** - a device-independent unit, because this
build host has a hardware double FPU and fast RAM and cannot measure the P4.

| | correlations | reference stations |
|---|---|---|
| morning (the 23-station build) | 14,282 | 23 |
| after all four | **6,215** | **24** |

**1. `extract_tone_powers` read the baseband once PER TONE.** It is the hottest
function in the receiver - ~150 calls per candidate, each 162 x 4 x 256
multiply-accumulates - and the baseband is 360 KB of `malloc`'d float, which on
this board means PSRAM. So three quarters of the decoder's PSRAM traffic was
re-reading samples it had just read. Sample-outer with four accumulator pairs
is **bit-identical**: each accumulator sums the same products in the same
order. That was the requirement, not a bonus - WSPR has no CRC, so the
agreement check is the only thing standing between us and a wrong codeword, and
its thresholds are tuned to measured score distributions. A change to the hot
path that moved those scores "a little" would invalidate them silently.

**2. `build_tone_tw` did 2048 DOUBLE trig calls, 32 times per candidate.** The
same trap `wspr_subtract` hit at 67 s per signal, hiding in a function nobody
had looked at because it only fills a small table. Found by ACCOUNTING against
the device's own phase timings rather than by reading code: `curve` reported
~1290 ms while running only 30 correlations worth ~520 ms, and the missing
~770 ms over 30 builds is ~26 ms each, i.e. **~12.7 us per double trig call -
the same per-call figure `wspr_subtract` measured independently.** Two paths
agreeing on that number is what makes it a result rather than a guess. Replaced
with a phasor recurrence re-seeded exactly every 64 samples: 40 trig calls
instead of 2048.

**3. The coarse start-time scan runs at stride 4.** It is ~111 of the ~150
correlations a candidate costs, and all it has to do is pick the right eighth
of a symbol - the fine refinement does the precision work at full rate.

- Legal ONLY because `mix_decimate` has already low-passed the baseband: at
  stride 4 the folding frequency is 46.9 Hz, inside the filter's stopband. It
  is the filter, not the arithmetic, that makes the shortcut safe, and it has
  to be re-checked if the filter moves.
- A strided score must never be compared with a full-rate one. `refine_dt` only
  replaces its incumbent when a trial BEATS it, so seeding it with a strided
  high-water mark would let the coarse pass veto every full-rate trial - the
  refinement would silently stop refining while still looking like it ran.
  Hence one full-rate re-score at the winning start time, which is also what
  the sync gate then reads.

**4. The decimation filter was 60 % longer than its job needs** - and this one
is the interesting one.

### The filter had never been sized against what it must reject

`mix_decimate` is ~1060 ms per candidate on the device and ~90 % of that is the
filter, so its length is the largest fixed cost in the receiver. 256 taps at a
50 Hz cutoff was chosen to be "enormously generous to the signal", which it is.

What it actually has to reject is narrower than that. Decimating 12000 to 375 Hz
folds input frequency f onto f mod 375, and **the decoder only ever READS
+/-34 Hz** of the result: the four tones live within +/-4.4 Hz, the frequency
search adds +/-1.5, and `measure_noise_ref` samples out to +/-34. So the only
content that can contaminate a decode is what lands in +/-34 Hz - the input
bands `k*375 +/- 34`, i.e. 341-409 Hz, 716-784 Hz, on to Nyquist. Everything
from ~40 Hz to ~341 Hz aliases into a part of the decimated band that **nothing
reads**. That slack is what 256 taps was paying for and not using.

### The paper answer was wrong, which is the lesson

`scratchpad/lpf_design.py` mirrors `build_lpf()` and computes the response over
every folding band to Nyquist. On paper **128 taps at 100 Hz is strictly better
than 256 at 50 on both axes at half the cost** - less passband droop AND better
alias rejection. Measured, it silently trades PA2PGU for 5B4AHZ.

A response curve does not predict which marginal stations survive it, because a
marginal decode turns on where the aliases land relative to that one signal.
This file already records PA2PGU's decode depending on aliasing from 188 Hz
away; it is the canary. Had the paper answer shipped, the station count would
still have read 23 and the SET would have changed underneath it.

So it was swept:

| taps | cut | droop at 34 Hz | worst alias | cost | stations |
|---|---|---|---|---|---|
| 256 | 50 | -2.17 dB | -65.5 dB | 1.00x | 23 |
| 160 | 100 | -0.58 dB | -53.6 dB | 0.62x | **24** (+5B4AHZ) |
| 152 | 100 | -0.58 dB | -55.9 dB | 0.59x | 24 |
| 144 | 100 | -0.57 dB | -58.0 dB | 0.56x | 23 (-PA2PGU) |

152 is the cliff. **160 ships, one step back from it** - 5 % of a filter is a
poor price for standing on an edge that some unrelated front-end change could
push us over.

The passband win is real too: four times less droop where the noise reference
is sampled. The old filter was quietly attenuating its own noise samples by up
to 2.2 dB, which biases the measured floor DOWN and every reported SNR UP.
Reported SNRs fall by about 1 dB as a result, which is the more honest number.

The tap count is now decoupled from the ring size. The ring index is masked
because a modulo here measured ~40 s per candidate - but that constraint
belongs to the RING, and tying the two together left only 256 or 64 reachable
when the answer was in between. Verified by rebuilding at 256 taps and
reproducing the old baseline with nothing lost or gained.

### Sensitivity: unchanged, and measured that way

Same-session noise-ladder A/B, both arms rebuilt from the same tree, two files:

| | 260824_1910 | 260824_1906 |
|---|---|---|
| before (256/50) | deficit ~2 dB, 0 fabrications | deficit ~2 dB, 0 fabrications |
| after (160/100) | deficit ~2 dB, 0 fabrications | deficit ~2 dB, 0 fabrications |

One marginal decode differs: at +2 dB of added noise on 1910 the old filter
kept 2 confirmed stations and the new one keeps 1. That is a single station at
one noise step and should not be read as a trend.

**The ladder runs a PREBUILT `wspr_cap_sweep` and does not rebuild it.** A
binary four hours stale produced a result that looked perfectly reasonable and
described code that was no longer there. Rebuild it per arm;
`scratchpad/ladder_ab.sh` does, and prints the binary's timestamp so the check
is visible rather than remembered.

### The candidate cap stays at 20, measured

Every cycle on air reports exactly 20 candidates, so the cap is saturated 100 %
of the time - the same shape as the old "8 candidates" ceiling, and a fair
reason to suspect it. With the speed work making room, it was swept:

| cap | 20 | 24 | 32 | 48 |
|---|---|---|---|---|
| stations found | 24 | 24 | 24 | 24 |

Not one extra station, for 95 % more correlation work. What the reference files
**cannot** say is whether that holds on a genuinely crowded band; four
recordings from two sites is a thin sample, and the comb ranks by energy rather
than SNR. If a real session ever shows stations appearing only when the cap is
lifted, raise it then - not on the strength of a reference file that says no.

### None of the millisecond figures here are measured on the new build

The device has not been flashed - the QMX needs a hand on its power switch
after every flash, and the operator was out. Every per-phase saving above is a
PROJECTION from the old build's `[mix + coarse + curve + dec ms]` line. That
same line is what will confirm or refute it. Remember also that per-candidate
timing is BOOT-SPECIFIC to about 35 %, so the comparison has to be made against
a figure from the same boot, not against numbers recorded earlier in this
document.

## The next big cost win, worked out but NOT done: a shared front end

After the changes above, `mix_decimate` is the largest single item again -
projected ~660 ms of a ~2600 ms decoded candidate - and it is doing the same
work twenty times over.

Every candidate mixes and decimates the **whole 120 s capture** from 12 kHz to
375 Hz, on its own. But all twenty candidates share one recording and sit
inside a ~300 Hz window. A two-stage decimation does the expensive part once:

- **Shared stage, once per cycle.** Mix to the middle of the candidate window,
  low-pass and decimate by 8 to 1500 Hz. Must preserve the whole candidate
  window (+/-150 Hz) plus the +/-34 Hz the decoder reads, so passband +/-184 Hz
  and stopband from 1316 Hz (the first band that folds into it at a
  decimate-by-8). Transition 0.094 normalised, so ~48 taps is generous.
  Cost: 48 x 180,000 = **8.6 M MAC, once.**

- **Per candidate.** Mix the residual offset (at most +/-150 Hz) and decimate
  by 4 to 375 Hz, on 180,000 samples instead of 1.44 M. The filter here is
  short: at a 1500 Hz input rate the stopband starts at 341 Hz (0.227
  normalised) against a 34 Hz passband, a transition of 0.204, so ~16 taps is
  enough and 24 is comfortable.
  Cost: 24 x 45,000 = **1.08 M MAC per candidate.**

Against today's single stage at 160 taps (160 x 45,000 = 7.2 M per candidate):

| | 20 candidates |
|---|---|
| now | 144 M MAC |
| two-stage | 8.6 M + 20 x 1.08 M = **30 M MAC** |

Roughly **4.8x** on the largest remaining item, and the per-candidate mixing
oscillator gets 8x cheaper too because it runs over 180,000 samples instead of
1.44 M.

### Why it is not done here

Because of what the filter sweep above just demonstrated. A front-end change
that is strictly better on paper moved which marginal stations decode - 128
taps at 100 Hz beat 256 at 50 on both computed axes and silently traded PA2PGU
for 5B4AHZ. A two-stage chain is a much bigger change to the same part of the
signal path, so it needs the same treatment: sweep the two filter lengths
against the reference set, watch the station SET rather than the count, and run
the noise ladder both sides.

That is a session's work with the radio available to confirm the projected
milliseconds afterwards, not something to leave unflashed and unmeasured.

⚠ It also touches `wspr_subtract`, which currently works on the raw 12 kHz
samples. If the shared stage becomes the thing candidates are decoded from,
subtraction has to happen somewhere both passes agree on - decide that before
writing any of it, because getting it wrong would make the second pass subtract
from audio the first pass never saw.

## Reproducing

Reference decoder, for the ground-truth list:

    for f in test/wav_reference/wspr/*.wav; do wsprd -f 14.0956 $f; done

Ours, linking the real decoder (build line also in `test/wspr_cap_sweep.c`):

    gcc -O2 -I main -I components/ft8_lib -o wspr_cap_sweep test/wspr_cap_sweep.c main/wspr_proto.c main/wspr_fano.c main/wspr_decode.c main/wspr_subtract.c components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c -lm
    ./wspr_cap_sweep test/wav_reference/wspr/260824_1910.wav 20

Regenerating the metric table:

    python tools/gen_wspr_metric.py > main/wspr_metric_table.h
