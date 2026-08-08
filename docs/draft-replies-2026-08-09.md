# Draft replies, groups.io, 2026-08-09

Two replies, both in the panadapter topic, both about binaural CW. Nothing
posts automatically.

Note for the operator: both of these are DESIGN conversations about something
that is not built and is blocked behind the CW-audio pipeline rework. Neither
promises a date, and neither should.

---

## To Don N2VGU — make the sound stage configurable?

Don — yes, and I think you have put your finger on why it has to be.

The width of the stage is not a constant anybody can pick correctly in advance.
It depends on headphones versus a speaker, on how far apart signals are on the
band, and on what you are doing: a CW operator picking one caller out of a
pile-up wants the stage stretched so two stations 50 Hz apart land in clearly
different places, while an SSB listener wants a single voice sitting naturally
in front of them rather than smeared across their head.

So it becomes a setting rather than a number in the code. My present thinking is
a stage width expressed in Hz — how much of the band maps across the full left-
to-right sweep — defaulting to the filter width, with narrower options for
pulling close signals apart. Michael suggested the same thing independently in
the same thread, arriving from the pile-up end rather than the phones-versus-
speakers end, which makes me fairly confident it is the right control.

What I would not do is expose the phase angle itself. It is the wrong unit for
the operator: nobody wants 45 degrees, they want "spread these two apart".

None of this is built. The DSP is genuinely small, but it needs an audio output
path on the Tab5 and that is currently switched off, so this is design talk and
I would rather say so than imply a date.

## To Michael KZ4LY — you had it right on the stage, and RIT is dropped

Michael — no correction needed on your part, and thank you for the clean summary
of the choice. You have described what I meant better than I did: position
follows where a signal sits, so a station keeps its place while you tune and the
stage stays still, rather than sliding across your head and wrapping around.

Your instinct about the disorientation is the argument I did not make. If the
mapping followed audio pitch, a station tuned through would exit one ear and
reappear in the other, which is a strange thing to do to somebody's spatial
hearing for no gain.

On stopping the task entirely in the digital modes: yes, that is possible, and
it is the right shape. The detail that decides whether it works is that the old
damage came from the output task merely existing — it runs above the FFT
consumer and on the same core, so waking every 120 ms and going straight back to
sleep it interrupted the audio pipeline about 125 times per FT8 slot with the
audio silent. Muting would change nothing; it has to not run at all.

Your stage-width idea is the same one Don arrived at from the other direction,
so it is now the control I plan to build: a width in Hz, defaulting to the
filter width, with narrower settings to pull close signals apart. Worth
experimenting with, as you say — which is exactly why it should not be a
constant.

RIT is dropped from the list on your word. Useful to know before building it
rather than after.
