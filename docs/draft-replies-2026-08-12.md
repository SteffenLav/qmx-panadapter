# Draft replies — groups.io, 2026-08-12

Not sent. One post per person. v1.8.1 is referred to as coming soon.

Accuracy notes for me, not for posting:
- Verified on hardware: RF gain tracking, CW centre range + seeding, split VFO B
  restore, Tab5 click-to-tune, web spot hit-test, the >1 KB settings save.
- Fixed in code but not separately tested: the QMX volume slider (same fault as RF
  gain, same fix).
- Built but never performed by a human: the seconds gesture.
- NOT fixed: the BT mouse (reverted), the QMX dual-VFO display (radio side), the
  USB wedge after long sessions.

---

## 1 — Samuel W7STF

Samuel, thanks for the list. Most of it turned out to be real bugs. All the fixes
below are in v1.8.1, which I will release shortly.

**QMX RF gain not matching between the Tab5 and the web page.** Fixed. The number was
read from a cache that only updated when the radio answered a query. So whichever
screen you opened second showed the old value. It now updates at once and asks the
radio to confirm. The volume slider had the same fault and is fixed too.

**CW centre stopping at 600 Hz.** Fixed. It is the sidetone. With the QMX default the
filter centre moves the CW offset and the sidetone with it. My slider was simply
wrong. The radio uses 500 to 950 Hz in 25 Hz steps. Which of those you can pick
depends on the CW filter width you have chosen. My slider ran 600 to 800 in 50 Hz
steps. It now covers the full range in the right steps. Your 550 works.

There was a second half to it. The Tab5 pushed its stored value to the radio about
four seconds into boot. The radio is not reachable over CAT until about seventeen
seconds. So that write never arrived. The Tab5 now reads the value from the radio
instead.

**Tuning on the web page.** Fixed. Each spot label was a click target across its whole
width. A callsign is often 3 to 4 kHz wide on screen at full zoom. The spot frequency
sits somewhere in the middle. So clicking a signal a couple of kHz from a spotted
station took you to the station. I reproduced it here. I aimed at 14.031.750 and
landed on 14.033.000. A label is now only a target close to the frequency it marks.
Everywhere else tunes to where you clicked.

**Something you did not report.** On the Tab5 most of the spectrum had stopped
responding to tap-to-tune. Only a window in the middle worked. A change last week
made the top bar touch areas shallower so they would stop swallowing taps on the spot
callsigns. The tune code still assumed the old deeper areas. So a band of the spectrum
belonged to nobody. Nothing opened and nothing tuned. It is fixed. The rule now is
simple. If the mouse pointer is white then clicking tunes.

**The Bluetooth mouse.** I found the main fault. The Tab5 asks the mouse to describe
its own layout and was only reading the first 22 bytes of the answer. The rest was
thrown away. So the description never made sense and the Tab5 fell back to guessing.
On my mouse the real description is 110 bytes and the layout then comes out right.

I have to be straight with you. My rewrite broke the mouse in a different way so I
took it out again. **The mouse is unchanged in v1.8.1.** I will redo it properly.

When I do I would like a diagnostic log from your mouse before I claim anything. Your
symptom does not match the fault I found. You describe the pointer hopping left and
right with vertical roughly usable. The fault I fixed does the opposite. So there is
probably a second one on your mouse.

**The disconnect after an hour.** That is on the radio side of the USB link. Once it
happens the QMX stops answering the first question a USB host asks. Only a power cycle
of the radio clears it. I tried six different approaches from the Tab5 end. One of
them held the USB power off through an entire reboot. None of them helped. I have
written it up for the QRP Labs side.

**The RIT button.** Fair point. v1.8.1 has a **Show RIT button** checkbox in the
drawer under Radio. It is on by default. Turn it off and the corner is yours. One
exception. If RIT is actually engaged the button comes back whatever the setting says.
I am not willing to have the radio listening off frequency with nothing on screen
saying so.

**The top bar labels and matching the web layout.** I am leaving these alone. The same
bar is used on the panadapter, in FT8 and on the CW page I am working on. Its
positions are muscle memory for anyone who has used it a while. The space saved is not
worth that.

**Arrow buttons beside the frequency.** I like it. What step would you want per press?
The current tuning step for the mode you are in, or a fixed one you choose?

73 Steffen

---

## 2 — Roy KI0ER

Roy, all four of yours were worth chasing. The fixes are in v1.8.1, coming shortly.

**The transmit offset leaving VFO B behind.** You were right that something was left
over. You were wrong about the consequence and that is the good news. The split was
switched off when you changed to FT8. Your FT8 was not going out 60 Hz high. What was
left behind was VFO B. It stayed at A plus 60 for the rest of the session. That is
what your radio was showing you.

The Tab5 now puts VFO B back to match VFO A before it drops the split. Then it asks
the radio to confirm simplex. The order matters. The QMX will not accept a new VFO B
once the split is already off.

**One part I could not solve.** The QMX still shows both VFOs afterwards. I can read
the radio's own display over CAT now so I can prove it. The radio reports VFO A mode,
transmit on A, split off. The display disagrees. Only a full configuration reload or a
power cycle puts it back to one VFO. The reload also switches off the radio's IQ mode
which would cost you the spectrum. That is too high a price for a display, so I left
it alone.

What matters is that the radio is not transmitting off frequency. VFO B equals VFO A
and the split is off. I check both by reading them back.

I am writing this up for Stan to verify and pass on. It looks like the display is not
redrawn when the VFO mode returns to A over CAT.

**VFO B not following the tuning knob.** That is intended. The offset only applies in
CW. In any other mode the Tab5 stops maintaining VFO B and it stays put.

**CW centre 625 and resetting to 700.** Fixed, and your suggestion was the right one.
Two faults. My slider only offered 50 Hz steps from 600 to 800. The radio uses 25 Hz
steps from 500 to 950. And the Tab5 pushed its stored value to the radio thirteen
seconds before the CAT link exists, so that write went nowhere every boot. The Tab5
now reads the centre from the radio at connect. The two cannot disagree any more and
625 is selectable.

**The mice.** Your MX Master works because it is Bluetooth Low Energy. Your two
Microsoft mice are almost certainly Bluetooth Classic. The Tab5 gets its Bluetooth
from a co-processor with no Classic radio in it. So you are right that there is
nothing to be done. It is not software. For anyone buying a mouse for this, look for
Bluetooth 4.0 or later.

73 Steffen

---

## 3 — Don WB0LQW

Don, you found a real gap and it is fixed in v1.8.1, coming shortly.

You are right that section 7.5 of the guide describes something that no longer
existed. HH and MM were editable and the seconds were not. So with WiFi off and no GPS
there was no way to get the clock inside the second that FT8 needs. That is exactly
where you were.

Roy's suggestion is what I built. Press and hold the SS box then release exactly on
the minute. The clock is set the moment you release. Not when you tap Save. The
release is the measurement. A Save a few seconds later would be a few seconds late.
Use your watch or listen for the FT8 gap. If you have typed HH and MM first it uses
those. Otherwise it takes the nearest minute to the Tab5 clock. While you hold it the
box turns amber and says what releasing will do.

Nobody has actually done this yet. It is a timing gesture and it needs a person with a
watch. So please tell me how it behaves rather than take my word for it.

I will bring sections 7.5 and 7.6 of the guide up to date.

You also mentioned the SS frame never going from blue to grey and the seconds
disagreeing with your watch. I have not looked at that yet and I do not want to guess.
Next time you see it, could you tell me whether WiFi was connected, what colour the SS
frame was, and roughly how far out the seconds were?

73 Steffen

---

## 4 — Brian WA6JFK

Brian, yes the GPS antenna needs to be connected. The QMX+ cannot get a fix without
it. The Tab5 only reads the time the radio already has.

On the radio, in the QMX+ menus:

- **GPS source** set to *QMX+ Internal* if you have the internal QLG3 fitted. The
  default is *Paddle port* which is for an external GPS on a plain QMX.
- **Clock** set to *ON* so the radio keeps and shows a real time clock.
- **Real time clock** set to *QMX+ Internal* if you have fitted the CR2032 cell. Then
  the clock keeps running while the radio is off. Otherwise leave it on *Software*
  which works but does not survive a power cycle.

There is no calibration or beacon menu to enter. The QMX reads the GPS data
automatically as soon as it arrives and sets its clock. Give it a few minutes for a
first fix from cold.

Nothing to configure on the Tab5. It reads the radio's time over CAT and works out for
itself whether that time comes from GPS.

One thing worth knowing if you ever use a plain QMX instead of a QMX+. There the GPS
goes on the paddle port. The GPS shares those pins with the key so the radio puts
itself in practice mode while it is connected. You will see a G on the display. That
is deliberate and it protects the transmitter. Connect the GPS, wait for the clock,
then disconnect it.

73 Steffen
