# Draft replies — groups.io, 2026-08-12

Not sent. Four posts. Everything below is what I can actually stand behind; where
something is unverified or unfixed it says so.

---

## 1 — Samuel W7STF (v1.8.0 feedback)

Samuel, thanks — most of this turned out to be real bugs rather than preferences.

**QMX RF gain not tracking between the Tab5 and the web UI.** Fixed. The value was
read from a cache that only updated when the radio answered a query, so whichever
screen you opened second showed the number from before your change. It now updates
immediately and asks the radio to confirm. While fixing it I found the volume
slider had the same fault, so that is fixed too.

**CW centre, and why it stopped at 600 Hz.** Yes, it is effectively the sidetone:
with the QMX's default Auto-offset/tone setting, changing the filter centre moves
the CW offset and the sidetone with it. My slider was simply wrong. The radio's
real centres are 500 to 950 Hz in **25 Hz** steps, and which ones are available
depends on the CW passband you have selected — with a 150 Hz filter, for instance,
the centres are 575, 625, 675 and so on. My slider ran 600 to 800 in 50 Hz steps,
so it was short at both ends and on a grid the radio does not use. It now covers
the whole range on the correct grid, so your 550 is reachable.

There was a second half to that: the Tab5 pushed its stored value to the radio
about four seconds into boot, but the radio is not reachable over CAT until about
seventeen seconds. So that write never arrived, on every single boot, and the two
numbers were free to disagree all session. The Tab5 now takes the value **from the
radio** when the link comes up.

**The web UI tuning problem.** Found and fixed, and it was not your imagination
being inconsistent. Each spot label was a click target across its whole width — a
callsign like OK/DL4ROB/P is 93 pixels, which at ×1 is 3.8 kHz of band, with the
spot's actual frequency merely somewhere in the middle. So clicking a signal a
couple of kHz from a spotted station tuned you to the station instead. I reproduced
it: aimed at 14.031.750, landed on 14.033.000. A label is now only a target within
about 400 Hz of the frequency it marks, and everywhere else tunes to where you
clicked. Tested here and behaving.

**And a second one you did not report, found while chasing yours.** On the Tab5,
most of the spectrum had stopped responding to tap-to-tune: only a window in the
middle worked, which is why it felt like it only tuned near the centre frequency.
A change last week shortened the top-bar Band/Mode/BW/Zoom touch areas so they
would stop swallowing taps on the spot callsigns, but the tune code still assumed
the old, deeper areas — so a band of the spectrum belonged to nobody. No menu
opened and no tune happened. It is fixed, and the rule is now simply that if the
mouse pointer is white, clicking tunes. You may not have noticed it with a finger;
with a mouse it is obvious, so it is probably worth a look next time you are on the
panadapter screen.

**The Bluetooth mouse.** I found the main fault: the Tab5 asks the mouse to describe
its own button and movement layout, and was only reading the first 22 bytes of that
description. The rest was silently discarded, so the description never made sense
and the Tab5 fell back to guessing from a layout I captured off one particular mouse
months ago. On mine the real description is 110 bytes, and with all of it the layout
comes out exactly right.

I have to be straight with you: my rewrite also broke the mouse in a different way,
so I have reverted it for now and will redo it properly. **So this is not yet fixed
in anything you can install.** When it is, I would like a diagnostic log from your
mouse before I claim it works — your symptom (hopping left and right, vertical
roughly usable) does not match the arithmetic of the fault I found, so there may be
a second one.

**The disconnect after an hour.** That is a known one, and it is on the radio's side
of the USB link, not the Tab5's. Once it happens the QMX stops answering the very
first question a USB host asks, and only a power cycle to the radio clears it. I
tried six different approaches from the Tab5 end, including holding the USB power
off through a whole reboot, and every one failed the same way. I have written it up
for Hans.

**The RIT button.** Fair point. There is now a **Show RIT button** checkbox in the
drawer under Radio, on by default, so you can turn it off and get the corner back.
One deliberate exception: if RIT is actually engaged, the button reappears whatever
the setting says — I am not willing to have the radio listening off frequency with
nothing on screen telling you.

**The top bar labels and matching the web layout.** I am going to leave these. The
same bar is used on the panadapter, in FT8 and on the CW page I am working on, and
its positions are muscle memory for people who have been using it a while. The space
saved would not buy back what changing it costs.

**Arrow buttons beside the frequency.** I like the idea. What step would you want per
press — the current tuning step for the mode you are in (250 Hz on SSB, 500 Hz on
digital), or a fixed one you set? And would a fast pair alongside them be useful, or
just the two?

73 Steffen

---

## 2 — Roy KI0ER

Roy, all four of yours were worth chasing.

**The transmit offset leaving VFO B behind.** You were right that something was left
over, and wrong about the consequence, which is the good news: the split *was* being
switched off when you changed to FT8, so your FT8 transmissions were not going out
60 Hz high. What was left behind was VFO B itself, still sitting at A+60 for the rest
of the session, which is what your radio was showing you. The Tab5 now writes VFO B
back to match VFO A before it drops the split, and then asks the radio to confirm it
is back to simplex. It has to be done in that order — the QMX will not accept a new
VFO B once the split is already off.

**One part I have not solved.** After all that, my QMX still displays both VFOs, even
though it reports that it is in VFO A mode, transmitting on A, with split off. I
cannot find any command in the CAT manual that addresses the display, so this may
simply not be reachable from outside. Does yours do the same? And if you press the
radio's Exit button once, does it change what is shown? That would tell me whether
the radio is really in the mode it claims.

**VFO B not following the tuning knob.** That one is working as intended. The offset
only applies in CW, so as soon as you are in another mode the Tab5 stops maintaining
VFO B and it stays where it was.

**CW centre 625, and resetting to 700.** Fixed, and your suggestion was the right
one. Two faults: my slider only offered 50 Hz steps between 600 and 800, when the
radio's real centres are 500 to 950 in 25 Hz steps; and the Tab5 pushed its stored
value to the radio about thirteen seconds before the CAT link exists, so that write
went nowhere every boot. The Tab5 now reads the centre **from the radio** at connect
and uses that, so the two cannot disagree, and 625 is selectable.

**The mice.** Your MX Master works because it is Bluetooth Low Energy. Your two
Microsoft mice are almost certainly Bluetooth Classic, which is the older kind — and
the Tab5's Bluetooth comes from a co-processor that has no Classic radio in it at
all. So you are right that there is nothing to be done: not a software limitation,
just silicon. For anyone buying a mouse for this, the thing to look for is Bluetooth
4.0 or later.

73 Steffen

---

## 3 — Don WB0LQW (and Roy) — setting the clock

Don, you found a real gap and it is fixed.

You are right that section 7.5 of the guide describes something that no longer
existed. HH and MM were editable and the seconds were not, so with WiFi off and no
GPS there was no way to get the clock inside the second that FT8 needs. That is
exactly the situation you were in, and there was no way out of it.

Roy's suggestion is what I have built: **press and hold the SS box, then release
exactly on the minute.** The clock is set the moment you release, not when you tap
Save — the release is the measurement, and a Save a few seconds later would be a few
seconds late. Use your watch, or listen for the FT8 gap. If you have typed HH and MM
in first it uses those; otherwise it takes the nearest minute to the Tab5's own
clock. While you are holding it, the box turns amber and says what releasing will do.

**Nobody has actually performed this yet** — it is a timing gesture and it needs a
person with a watch, so I would rather you told me how it behaves than have me claim
it is right.

I will bring sections 7.5 and 7.6 of the guide up to date with this.

Separately, you mentioned the SS frame never going from blue to grey, and the seconds
disagreeing with your atomic watch. I have not looked into that yet and I do not want
to guess at it. When you next see it, could you tell me whether WiFi was connected at
the time, what colour the SS frame was, and roughly how far out the seconds were?

73 Steffen

---

## 4 — Brian WA6JFK — GPS time on the QMX+

Brian, yes, the GPS antenna needs to be connected — the QMX+ cannot get a fix
without it, and the Tab5 only ever reads the time the radio already has.

On the radio, in the QMX+ menus:

- **GPS source** — set to *QMX+ Internal* if you have the internal QLG3 fitted
  (the default is *Paddle port*, which is for an external GPS on a plain QMX)
- **Clock** — set to *ON*, so the radio keeps and displays a real time clock
- **Real time clock** — *QMX+ Internal* if you have fitted the CR2032 coin cell, in
  which case the clock keeps running while the radio is off. Otherwise leave it on
  *Software*, which works but does not survive a power cycle

There is no calibration or beacon menu to enter: the QMX parses the GPS data
automatically as soon as it arrives, and updates its clock. Give it a few minutes
for a first fix from cold.

Nothing needs configuring on the Tab5. It reads the radio's time over CAT and works
out for itself whether that time is GPS-disciplined, by checking how closely the
radio's seconds agree with internet time when both are available.

One thing worth knowing if you ever use a plain QMX rather than a QMX+: there the GPS
goes on the paddle port, and because the GPS shares those pins with the key the radio
puts itself into practice mode while it is connected — you will see a G on the
display. That is deliberate, to protect the transmitter. You connect the GPS, wait
for the clock to update, then disconnect it.

73 Steffen
