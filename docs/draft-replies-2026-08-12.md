# Draft replies — groups.io, 2026-08-12

Not sent. One post per person.

Accuracy notes for me, not for posting:
- Verified: RF gain tracking, CW centre range + seeding, split VFO B restore, Tab5
  click-to-tune, web spot hit-test, the >1 KB settings save.
- Fixed but not separately tested: the QMX volume slider (same fault as RF gain).
- Built, never performed by a human: the seconds gesture.
- NOT fixed: the BT mouse (reverted), the QMX dual-VFO display (radio side), the USB
  wedge after long sessions.

---

## 1 — Samuel W7STF

Samuel, thanks. Most of that was real bugs. All fixed in v1.8.1 unless I say
otherwise.

**RF gain not matching between Tab5 and web page.** Fixed. Both read a stale cache.
The volume slider had the same fault.

**CW centre stopping at 600 Hz.** Fixed. It is the sidetone. The radio does 500 to
950 Hz in 25 Hz steps, and which ones you can pick depends on your CW filter width.
My slider had the wrong range and the wrong step. Your 550 works now. The Tab5 also
reads the centre from the radio at connect, so the two cannot disagree.

**Tuning on the web page.** Fixed. Each spot label was a click target across its whole
width, so clicking a signal near a spotted station took you to the station. A label is
now only a target close to the frequency it marks.

**One you did not report.** On the Tab5 most of the spectrum had stopped responding to
tap-to-tune. Fixed. The rule now is that if the mouse pointer is white, clicking tunes.

**Bluetooth mouse.** I found the main fault but my fix broke something else, so I took
it out. **The mouse is unchanged in v1.8.1.** I will redo it and I will ask you for a
diagnostic log first. Your symptom does not match the fault I found, so there is
probably a second one on your mouse.

**Disconnect after an hour.** That is the radio side of the USB link. Only a power
cycle of the QMX clears it. I tried six approaches from the Tab5 end and none helped.
Reported to QRP Labs.

**RIT button.** v1.8.1 has a Show RIT button checkbox in the drawer under Radio, on by
default. Turn it off and the corner is yours. If RIT is actually engaged the button
comes back anyway.

**Top bar labels and matching the web layout.** Leaving these. The same bar is used on
the panadapter, in FT8 and on the CW page I am working on.

**Arrow buttons beside the frequency.** I like it. What step per press?

73 Steffen

---

## 2 — Roy KI0ER

Roy, all four were worth chasing. Fixed in v1.8.1 unless I say otherwise.

**The offset leaving VFO B behind.** The split was switched off when you changed to
FT8, so your FT8 was not going out 60 Hz high. VFO B was what got left behind. The
Tab5 now puts it back to match VFO A before dropping the split, then reads back to
confirm.

**The display still shows both VFOs.** That part I could not solve. The radio reports
VFO A and simplex while its display disagrees. Only a configuration reload or a power
cycle clears it, and the reload switches off IQ mode, which would cost you the
spectrum. So I left it. The radio is not transmitting off frequency. I am sending it
to Stan to verify and pass on.

**VFO B not following the tuning knob.** Intended. The offset only applies in CW.

**CW centre 625 and resetting to 700.** Fixed, and your suggestion was the right one.
The radio does 25 Hz steps from 500 to 950, and the Tab5 now reads the centre from the
radio instead of pushing its own. 625 works.

**The mice.** Your MX Master is Bluetooth Low Energy. The two Microsoft ones are almost
certainly Bluetooth Classic, and the Tab5's Bluetooth chip has no Classic radio. So
nothing can be done. For buying a mouse, look for Bluetooth 4.0 or later.

73 Steffen

---

## 3 — Don WB0LQW

Don, you found a real gap and it is fixed in v1.8.1.

You are right that section 7.5 describes something that no longer existed. HH and MM
were editable and the seconds were not, so with no WiFi and no GPS there was no way to
get inside the second FT8 needs.

Roy's suggestion is what I built. Hold the SS box and release on the minute. The clock
is set when you release, not when you tap Save. Use your watch or the FT8 gap.

Nobody has actually done this yet, so please tell me how it behaves. I will update
sections 7.5 and 7.6.

You also mentioned SS never going from blue to grey and the seconds disagreeing with
your watch. I have not looked at that yet. Next time you see it, could you tell me
whether WiFi was connected, what colour the SS frame was, and how far out the seconds
were?

73 Steffen

---

## 4 — Brian WA6JFK

Brian, yes the GPS antenna needs to be connected. The QMX+ cannot get a fix without
it, and the Tab5 only reads the time the radio already has.

In the QMX+ menus:

- **GPS source** to *QMX+ Internal* if you have the internal QLG3 fitted
- **Clock** to *ON*
- **Real time clock** to *QMX+ Internal* if you fitted the CR2032 cell, so the clock
  keeps running while the radio is off. Otherwise *Software*, which does not survive a
  power cycle.

There is no calibration menu to enter. The QMX reads the GPS automatically and sets
its clock. Give it a few minutes for a first fix from cold. Nothing to configure on
the Tab5.

On a plain QMX rather than a QMX+ the GPS goes on the paddle port and shares pins with
the key, so the radio drops into practice mode while it is connected and shows a G.
Connect it, wait for the clock, then disconnect.

73 Steffen
