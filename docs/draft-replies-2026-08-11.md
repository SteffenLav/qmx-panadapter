# Draft replies, groups.io, 2026-08-11

Three posts in the panadapter topic. Nothing posts automatically.

Notes for the operator before posting:

- **Reply 3 is the one that matters most to other people.** SWR protection set
  from the browser has never been saved on any v1.7.x build. Anyone who set it
  there and did not check it on the Tab5 is running without it. That is worth its
  own post rather than a line buried in a long reply.
- Nothing in these replies is released yet. They say "on my bench" and "the next
  release" and give no dates.
- Reply 1 asks Samuel four questions rather than announcing a fix, because I have
  not reproduced what he reported. The CW spot offset I *did* find is a constant
  shift and his report is asymmetric, so they may well be different things.
- Reply 2's mouse paragraph was **rewritten after the fix went in**. The first
  version said the old code decoded movement into "numbers in the thousands"; the
  host harness proved that wrong. What actually happens on a 16-bit mouse is that Y
  comes out ~16x too large while X is very nearly right by coincidence, and the 16x
  vertical gain is what pins the cursor to the top edge. The sharper version is both
  truer and more convincing, so the reply now leads with the fix rather than asking
  for a log.
- **The 60m label still needs his log** — that one is not diagnosed, only narrowed
  to "the radio reported the name as 0".

---

## Reply 1 — to Samuel W7STF and Roy KI0ER, on the tuning oddity

Samuel, Roy — thank you both. A report and a failed attempt to reproduce it are
more useful together than either alone, so Roy's "I could not make it happen" is
not a wasted post.

I have not reproduced it either. What I have done is go through the click-to-tune
path line by line against the Tab5's, and I found two real faults in the browser.
I want to be straight that neither of them looks like the thing Samuel described,
so I am not claiming this is solved.

The first one only bites in CW. Every spot label in the browser was being drawn one
CW pitch — 700 Hz with the default — to the left of the signal it was labelling.
That is 19 pixels at zoom x1 and 149 pixels at x8. The frequency-to-pixel routine
was subtracting the CW pitch, which double-counts a shift the Tab5 has already
applied by the time the spectrum data reaches your browser. The click path never
had that term, so drawing and clicking disagreed — in CW, and only in CW. Fixed.

The second is not CW-specific: the browser was rounding the tuning *offset* and
adding it to the dial, where the Tab5 rounds the absolute *target*. The practical
effect is that the grid sat wherever the VFO happened to be, so in SSB a click
could land up to 250 Hz from the signal you aimed at, and the grid moved again every
time you tuned. Its grid values had also drifted from the Tab5's — 500 Hz in SSB
where the Tab5 uses 250, 100 Hz in digital modes where the Tab5 uses 500. Both
fixed, and the two now share one piece of code so they cannot drift apart again.

In CW the grid is 10 Hz either way, which is why I do not think that second one is
your bug. And a constant 700 Hz shift should miss on both sides equally, where you
described the right side working and the left not — so I am missing something.

Four questions, and the third is the one I most want answered:

1. CW or CW-R?
2. What are your CW pitch and IF calibration set to?
3. Before you click, the cursor shows a frequency. Does that number match what the
   VFO reads after you click? If it does, the click is computing the right
   frequency and the display is drawing the signal in the wrong place. If it does
   not, it is the other way round. That single observation splits the problem in
   half.
4. Were you clicking on a spot label, or on a trace in the spectrum or waterfall?

If it turns out you were working from spot labels, the first fault above becomes a
strong candidate after all: clicking a label tunes to the station's real frequency,
so the radio would go where you expect while the trace you were looking at sat
somewhere else — which could read exactly like "the VFO says it is centred but the
signal is not there".

A diagnostic log covering a few of these clicks would help too. Files ->
"Diagnostic download" in the browser, and send me qmx-log.txt.

## Reply 2 — to Samuel W7STF, the rest of the list

Samuel — taking these in turn.

**The Bluetooth mouse.** Found it, and it is fixed on my bench. Your description
is what identified it, down to the detail about the cursor sitting at the top.

BLE mice do not agree on a byte layout for movement. The Tab5 was assuming one for
any report of five bytes or more — the one I captured off a Logitech M240 here,
which packs X and Y into twelve bits each and shares a byte between them. If your
mouse sends sixteen-bit movement instead, which is the other common choice, that
assumption reads Y four bits early: it picks up Y's low byte shifted up by four,
with a nibble of X leaking into the bottom bits. The arithmetic works out to Y being
about **sixteen times too large**, while X comes out very nearly correct by
coincidence.

Sixteen times gain on the vertical is exactly your symptom. Any movement at all
drives the pointer into the top or bottom edge, where it is clamped and stays — so
what is left is a cursor that only slides left and right along the very top of the
screen. The "it worked in X and Y for a short period" fits too: the mouse most
likely started in the simple boot format and switched to its own once it was fully
connected.

The fix is to stop assuming. Every HID mouse publishes a descriptor saying exactly
where X, Y and the wheel sit and how wide they are; the Tab5 was already reading
that descriptor and throwing it away, logging only how many bytes it was. It now
parses it and decodes accordingly, so this should work on your mouse and on models
neither of us has. I have tested the parser against three real descriptors,
including the two byte-for-byte captures from the mouse here, but I cannot test it
against *yours* — so if the pointer still misbehaves, one diagnostic log will now
tell me everything: it prints the descriptor as hex and the layout it derived from
it.

On speed: there is no sensitivity adjustment at all today — one unit of movement
from the mouse is one pixel on the screen. I suspect most of what you are calling
"uncontrollably fast" was the misreading rather than real sensitivity, so I would
like you to try it decoded properly first. If it is still too quick after that, say
so and I will add the setting; it is easy, I just do not want to add a knob to
compensate for a bug.

**60m showing as "0m".** The browser prints whatever name the Tab5 gives it with an
"m" after it, so the Tab5 is reporting that band's name as "0" rather than "60".
Those names are not built into my firmware — they are read out of your radio's own
band table over CAT, one slot at a time, and the length of each name is worked out
from the reply. My bench radio reports "60" correctly, so I cannot reproduce it —
but the Tab5 logs the name it got for every band slot, so a diagnostic log will show
me exactly what your radio answered. Files -> "Diagnostic download", and send me
qmx-log.txt whenever it is convenient. This is the one item on your list I have not
been able to get to the bottom of from here.

**CW decode.** Working as built, I am afraid — it is not in any release. What you
read is the roadmap. The QMX+ decodes internally and mirroring that over CAT is the
plan; I have the reader for it working on a development branch, but it is not
something you have.

**Release radio.** You are right and I will reword it. "Release radio to QMX menu"
and "Take radio back" are describing the mechanism rather than telling you what to
do, and the pause/play icons are borrowed from something they have nothing to do
with. On the placement: while it is released there is already a banner across the
screen you can tap to take the radio back, so the way *out* is at your fingertips —
it is the way *in* that is buried in settings. I will look at that.

**The web interface.** Yes, and rather more since v1.7.2. On my bench now: RIT, so
you can pull in an off-frequency caller from the browser by clicking them, with the
transmit frequency staying put and a marker showing where you are actually
listening; starting and stopping a POTA/SOTA activation, with a badge that appears
while one is running so you cannot forget it; and the last of the Tab5-only
settings — CW pitch, IF calibration, the battery charge limit, the 180-degree flip,
Fox/Hound, simulation mode and the spot mode filter.

One correction to your question, though, because it deserves an accurate answer
rather than a flattered one: no, I have not changed how black level and contrast
feed the browser's picture, and it is worth knowing that they do not. The browser
draws its own waterfall — it works out its own noise floor and uses its own fixed
colour ramp — so black level, contrast, adaptive floor and the colour scheme
currently only change what the Tab5's own screen looks like. The FFT window does
affect both, because that calculation happens on the Tab5 before anything is sent.
That is an inconsistency I had not noticed until you asked, and I will make the
browser follow those settings.

Thank you for the detail in that list. Reports written like that are the reason
several of these are getting fixed at all.

## Reply 3 — to everyone, one thing to check if you use the browser

A bug worth checking on your own unit, because it affects a protection feature and
it fails silently.

If you have ever set **SWR protection** from the browser's Settings page, it was
not saved. The field was shown, it accepted your value, it reported success, and
the device discarded it — the setting existed everywhere except in the code that
stores it. Reload the page and it would read back whatever it was before, which is
easy to miss.

So: if you rely on SWR protection, check it on the Tab5 itself — Settings, "SWR
protection (transmit)". If it says Off and you thought you had set it, that is this
bug and not you.

Setting it on the Tab5 has always worked correctly. Only the browser path was
affected, on every v1.7.x build. It is fixed on my bench, and in the next release
the browser will offer the same four thresholds the Tab5 does instead of asking you
to type a number.

My apologies — a control whose whole job is to stop a transmission into a bad
antenna is the last one that should fail quietly.
