# JS8 free-text reference corpus (KiwiSDR)

16 contiguous slots off a public KiwiSDR, 2026-10-09, **14.078 MHz**, receiver
`85.183.11.108:8073` (DF3LZ, Hamburg). Two complete free-text messages and the
directed exchange around them.

    ./js8_wav_harness test/wav_reference_js8_kiwi

## Why this exists, and why it is not off the bench

`../wav_reference_js8` is 12 slots off bench dev and **not one of them carries
a data frame** - 7.078 at that hour is the heartbeat network. So J7, the whole
free-text decoder, had no on-air regression floor at all. This directory is
that floor.

It came from `tools/js8_kiwi_slots.py`, which cuts a `kiwirecorder` recording
into corpus-shape slots. A public receiver gives traffic from wherever the
band is busy, independent of this antenna.

## ⛔ THE SLOTS MUST STAY CONTIGUOUS

A free-text run spans slots and `js8_reasm` closes it after
`JS8_REASM_TIMEOUT_SLOTS` (4 = 60 s) of quiet. The first version of this
corpus held only the slots that reported free text, which left a 105 s gap -
the run expired, and the tail of the message was silently lost. **Do not prune
a slot out of the middle**, even one that decodes nothing. The two blocks here
are `141145-141345` and `141645-141815`.

## What is in it

| Station | Message |
|---|---|
| F6HCM | `SQUARESDR2, 20W, EF ANTENNA 10/(15/)20/40M (OR  3 ELEM 15M) , MIMIZAN WEST FRANCE` |
| F4MEE | `JE NE M'ATTENDAI PAS A <?> UN FR SUR CETTE FRÉ<?> 73 CUAGN` |

plus `F6HCM F4MEE INFO?`, `F4MEE F6HCM INFO`, `F6HCM F4MEE ?`,
`HB F6HCM IN94` and `HB IU7VLD JN80`.

⭐ Both messages are the answer to an `INFO?` command, which is where free text
actually appears on the air. ⭐ `<?>` is a word above our truncated codebook;
it does **not** end the line, which was a deliberate J7 decision and this is
its first test against real traffic. ⭐ The accented `É` survives the
Latin-1-to-UTF-8 re-encoding.

## The .txt files

Same format as `../wav_reference_js8`, with one addition:

    #F <sender>: <text>

is an expected **free-text run** for that slot, exactly as
`js8_reasm_describe()` renders it. A run that does not come back is a LOST
decode and fails, the same contract the `~` rows have. It hides behind `#` so
an older harness reads it as a comment.

⚠ **This is this build's own output, not ground truth.** It is a regression
floor: decoding *more* is fine, decoding *less* fails. A message JS8Call would
have read and we did not is simply absent here.

⚠ A run is reported in **every slot it takes a frame in**, so a growing
message appears several times, each one longer. That is the same rule the
firmware's slot log follows. It is not duplication.

## What this corpus does and does not guard

Mutation-tested 2026-10-09, five ways:

| Mutation | Caught |
|---|---|
| text never accumulated | yes - 12 lost |
| duplicate candidates counted as fragments again | **yes - 12 lost** |
| accumulated text corrupted | yes - 12 lost |
| frequency tolerance set to 0 | no |
| run timeout cut to one slot | no |

The last two survive because **this recording does not exercise them**: the
candidates land on the same offset every slot, and a frame arrives in every
slot of each block, so neither the tolerance nor the timeout is ever the thing
that holds a run together. They are covered by
`test/js8_reasm_harness.c` instead. Said plainly here so nobody reads a pass
as more coverage than it is.
