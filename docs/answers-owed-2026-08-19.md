# Answers owed — drafted 2026-08-19, NOT posted

Eight questions that needed a fact checked rather than a guess. Each one below
was verified against the code tonight; the verification is noted for me, not for
posting.

---

## Samuel W7STF — why are there fewer spots on the Tab5 than in the browser? (#148)

*Verified: `MAX_LABELS 16` in `ui/spots_lane.c`; both screens read the same
`SPOTS_MAX 200` list.*

Both screens get exactly the same spots — the difference is purely how many the
Tab5 can draw.

The Tab5's lane has a fixed pool of 16 label widgets, and it only prints a
callsign where there is room for it without overlapping its neighbours. On a busy
band you will often see something like "17 visible, 7 drawn": all 17 are there and
tappable, only 7 got a name printed. The browser has a much wider canvas and no
widget pool, so it labels far more of them.

So nothing is being filtered out, and no spot is missing from the data. If the
crowding bothers you, zooming in spreads them out and more get labelled.

---

## Samuel W7STF — does the Smoothing slider actually do anything? (#150)

*Verified: `render.c` applies `s_ema_alpha` unconditionally every frame; both the
drawer slider and the web setting call `render_set_ema_alpha()`.*

It is real and it is wired. The spectrum runs
`smoothed = alpha * new + (1 - alpha) * smoothed` on every frame, with no
condition around it, and the slider reaches that value from both the Tab5 and the
web page.

At 1.00 there is no smoothing at all; at 0.05 the trace should be visibly
sluggish. So the extremes ought to be obvious.

What can hide it: on a quiet band there is little frame-to-frame variation to
smooth, which is much the same thing you suspected about the presets.

If you set it to 0.05 and then 1.00 on a band with activity and genuinely cannot
tell them apart, that is a real bug and I would like a screenshot of each — I can
only confirm the code path from here, not what your screen does.

---

## Samuel W7STF — spur suppression "doesn't seem all that effective" (#157)

Fair question, and the answer depends on which mode you used, because the two are
deliberately very different.

**Subtract** removes the measured power of the spur and nothing more — about
12–17 dB. It is the conservative choice and it *cannot* hide a real signal. If you
expected the spur to disappear, this mode was working as designed and the problem
is my wording rather than the DSP.

**Erase** interpolates the affected bins away. Measured on 20 m, where the comb
runs about 38 dB over the noise floor, it takes the spur columns down roughly 78%
on the waterfall against 28% for Subtract. That is the one to use.

The one thing more clever detection would not buy you: the ceiling is the spur's
own ~0.5 dB wobble. A constant cannot cancel something that moves. The detection
itself is physical rather than statistical — a 25 Hz dial nudge moves a spur 16 to
50 times as far as a real signal, which is an order of magnitude of margin.

Which mode were you on, and could you send a before/after screenshot? I would
rather measure your unit than redesign a detector that is not the limiting factor.

---

## Gyula HA3HZ — two stations answer my CQ; what happens to the second? (#158)

Both behaviours you saw are as designed, and you are right that nothing on screen
tells you so.

The second caller goes into the **pile-up list** and stays there. If you do
nothing, the firmware finishes with the first station and then calls CQ again —
it does not work the second one for you unless you turn on **Auto-work pileup**.
If you pick the second one by hand and they have gone, the exchange retries a few
times and then times out, which is the "reports x times and times out" you saw.

So neither is a fault, but the screen should say more than it does. I am treating
that as the real issue rather than the behaviour.

---

## Roy Ashkenaz K2RMA — spurs on my QMX but none on the QMX+? (#168)

That can be genuine, and it does not mean either radio is faulty.

The comb is generated inside the radio's own synthesiser, and how strong it is
depends on the dial frequency: there are only a few bad windows per 100 kHz, each
one or two kHz wide. 14.074 — the FT8 calling frequency — happens to sit inside
one of them on the unit I measured. A different radio, or the same radio on a
slightly different frequency, need not agree at all.

Which band and frequency was each radio on when you compared them? If both were on
14.074 that is a more interesting result than if they were a few kHz apart.

You can see them with the antenna disconnected, which is the quickest way to
confirm they are self-generated rather than something on the air.

---

## Roy Ashkenaz K2RMA — can the Tab5 dual-boot between this and Zhenxing's firmware? (#178)

*Verified against `partitions.csv`: one `factory` app partition, no OTA slots.*

No, and I do not plan to add it.

The Tab5 has a single application partition, so there is nowhere for a second
firmware to live. Adding one means changing the partition layout, which on an
already-flashed unit is the operation most likely to take your settings, your QSO
log and your LoTW key with it.

The practical answer is that reflashing already does this. The flasher takes about
15 seconds and a normal (non-erase) flash leaves your settings alone, so switching
back and forth is not much slower than a reboot would be.

---

## John W3JED — is there a video of the panadapter in action? (#179)

Not one I have made, no.

I would rather point you at something real than promise one I have not recorded.
The user guide at https://tab5.lav.dk has screenshots of every screen, and the
quick-start walks through a first session.

If anyone on the list has filmed theirs I would happily link it from the site.

---

## Gyula HA3HZ — is the web page the only way to download the log? (#213)

*Verified: `/api/adif` handler, the Tab5 ADIF viewer, and `SD_ADIF_PATH` in
`sd_archive.c`.*

No — there are three routes, and one of them needs no browser at all.

**The web page** — QSO Logs → *ADIF download*, or *Today only, dated file* which
names it `qso-YYYY-MM-DD.adi`. That second one is aimed exactly at what you
described: downloading each day's contacts separately.

**The Tab5 itself** — the ADIF-log button on the FT8 screen shows the log and lets
you filter to today.

**A microSD card** — put a card in before you switch the Tab5 on and the log is
mirrored to it automatically as `qmx-panadapter/qso.adi`, along with your config
and LoTW certificate. Take the card to a PC and the file is simply there. For a
daily download with no browser and no network, this is the least effort.

The card copy is kept up to date as you work, so you do not have to remember to
export anything.
