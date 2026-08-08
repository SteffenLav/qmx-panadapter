# Draft replies, groups.io, 2026-08-08

Five replies, each headed with WHERE it goes — they span two different threads,
so posting the right text in the right place matters. For the operator to review
and post; nothing posts automatically.

  1. Roy KI0ER            -> panadapter topic
  2. Michael KZ4LY        -> panadapter topic
  3. Samuel W7STF + Stan  -> Band config thread (#176111)
  4. Samuel W7STF alone   -> panadapter topic (Bluetooth mouse)
  5. BD4AHS               -> panadapter topic

There is a FIFTH, older and still unposted, in its own file:
docs/draft-reply-orderly-shutdown-2026-08-07.md (Roger AD5DZ and Stan, the
orderly-USB-shutdown experiment — a negative result).

Nothing here was posted on 2026-08-07, so the Roy reply below folds together his
FT8 evening batch AND his CW request from the following day.

---

## To Roy KI0ER — the FT8 batch, and the CW offset

Roy — two productive evenings on your end. FT8 first, then the CW one, which is
the more interesting of the two.

The dual EVEN/ODD strips are implemented, exactly as you argued them. Every
occupancy strip — the mini strip on the FT8 pane, the full picker, and the web
page's picker — now shows two rows, EVEN above ODD, always, never a combined
"BOTH". Your white marker sits on your own window's row once a transmission has
fixed it, and on both rows before that, because until then the tone is chosen but
the window is not. The picker's verdict says where you stand in words: "Clear in
EVEN — busy in ODD".

You also found a real bug in the same mail. The "FREQ BUSY" warning never
received the time-window filter the occupancy map got in v1.3.4 — it was still
counting stations in the opposite window, which cannot collide with you. That is
exactly why it contradicted the green strip. Fixed: it now judges only your own
window.

The auto-answer change is in too, on your reasoning verbatim: a robot pick that
turns out to be mid-QSO with somebody else is abandoned on the spot and the robot
chooses another CQ caller. A deliberate pounce still holds and waits — a willing
pounce means you want that station. No grey-list strike for the busy one; busy is
not unresponsive.

The blank message area: your QMX menu reproduction was the giveaway, and between
it and Samuel W7STF's Band config thread the mechanism is now clear. Stan KC7XE
identified the blank LCD and "Starting processes" that Samuel saw as the QMX's
own watchdog reset — so the radio is quietly restarting. Two consequences follow.
IQ mode on the QMX is *session* state, not a stored setting, so a restart
switches it off; and a QMX restart with the cable connected usually does not
produce any disconnect the Tab5 can see. Which leaves exactly what you reported:
a radio answering every CAT command perfectly, sending no audio. Transmit works,
nothing decodes, the rows age out after a minute, and only restarting the radio
by hand appears to fix it — because that finally forces a reconnection.

So the next build just asks again. After 30 seconds of silence with CAT still
answering, it re-sends the IQ-mode enable: free, invisible, and it should recover
your case on its own. Only if silence continues does it escalate to an audio
reset at 60 seconds and the USB power-cycle you perform by hand at 120, capped so
a genuinely dead radio cannot loop it. There is also now a "Release radio" button
that stops the Tab5 talking entirely while you use the QMX's menus — a gentler
version of your own advice to unplug the USB cable first. Your diagnostic logs
would still be very welcome to confirm the signature matches; if you see
"re-asserting IQ mode" in the log followed by decodes coming back, that is this
working.

Now the CW offset. This one is in, and I think you are right that it changes what
the device is for.

The QMX has no XIT of its own — its CAT manual is explicit, "XIT status: always 0
because QMX has no XIT" — so I did it the way you do it by hand: split, receive
on VFO A, transmit on VFO B, with B held at A plus your offset. The difference is
that the Tab5 maintains it. Set the offset once in Settings (centre of the slider
is off; 400–600 Hz is the useful range), and from then on every way the frequency
can move takes the offset with it: a tap on the panadapter, a spot click, a
memory recall, a band change, the web page, and the radio's own tuning knob. That
is your "I can change frequency to another station, and not touch anything else,
and the offset will follow", which is the part that makes it usable while
hunting.

Three deliberate limits. It applies in CW and CW-R only — an offset transmit in
SSB or a digital mode is a mistake, not a courtesy — and leaving CW clears it. It
only ever clears split if the Tab5 was the one that set it, so if you are running
your own split it will not interfere. And it re-asserts itself every 30 seconds,
so a radio that dropped split on its own (a band change, a menu visit) is brought
back into line rather than quietly transmitting on top of the DX.

What I cannot claim: I have not made a CW contact with it. The CAT side is
straightforward and the commands are in the 1.03 manual as well as 1.04, so this
works on the firmware I recommend — but you would be the first person to key it
in anger. If the offset lands the wrong way round, or the radio does something
unexpected coming out of split, tell me and I will fix it the same day.

Your CQ-pause idea (skip an occasional transmission during a long CQ run so your
own window's picture refreshes) is a good one and is on the list — it changes
on-air conduct, so I want to think about the cadence rather than rush it.

And thanks for the v1.04.005 note and the grey-list confirmation. 1,100 miles per
Watt through 100 feet of coax at 2:1 is not bad at all for a "CW operator".

## To Michael KZ4LY — XIT, and the binaural idea

Michael — thank you, both of those are worth answering properly.

On XIT: you are right that the QMX does not have it, and its CAT manual says so
in as many words. But the effect you want is available today, because the radio
does have split and CAT control of VFO B. The next build has a "CW transmit
offset" setting that holds VFO B at the receive frequency plus a chosen offset
and keeps it there as you tune around — CLAR TX in everything but name, without
waiting for Hans. If real XIT does land in QMX firmware with its own CAT command,
I would switch to it: it would be one write instead of two and would not touch
the VFOs at all. Until then, split does the job.

Worth noting the other half of what you described: the QMX already exposes RIT
over CAT (RT, RU, RD, and in 1.04 also RC and RR for the step rate), so your
"CLAR RX" for round-robin CW is a smaller job than XIT was. It is not built yet —
tell me if you would use it and I will put it in the CW page.

On binaural: I like it and I have not forgotten it. The reason it has not
happened is not the DSP. The Tab5 receives I and Q as separate channels, so a
45-degree phase split into left and right is a handful of lines. The blocker is
the Tab5's own audio *output* path — an earlier version of it competed with the
USB audio input badly enough to cut FT8 decode rates by half, so it is currently
switched off pending a rework of that pipeline. When that rework happens,
binaural comes with it, and doing it on the Tab5 has one advantage over doing it
in the radio: the Tab5 knows where every signal sits in the passband, so the
stereo image can follow the spectrum rather than being a fixed 45 degrees.

## To Samuel W7STF AND Stan KC7XE — post in the Band config thread (#176111)

Both of them are in that thread, and everything below is about what they raised
there. The Bluetooth mouse is NOT here — it belongs to the panadapter topic and
is a separate message to Samuel alone, further down.

Samuel, Stan — both of Stan's suggestions are in the next build.

**RF gain.** The QMX exposes it as RG, on 1.03 as well as 1.04, so no firmware
change is needed. It is a slider under the QMX volume in Settings, 0 to 99 dB,
radio default 54. It is a per-band value, so the Tab5 reads it from the radio
each time you open Settings, and the slider writes once, on release.

**Release radio.** Stops all CAT traffic so the QMX's menus and Terminal
Applications have the port to themselves. It also stands down the watchdogs,
which read a deliberate menu visit as a fault. A blue bar shows the state and
takes the radio back when tapped. Resuming re-checks IQ mode.

**The Band config reboot.** Stan's reading holds up. The front-panel menu drives
the display from the MCU and is not isolated from CAT the way the terminal menu
is, which is why PuTTY and USB-unplugged are both clean. The key part is the
blank LCD and "Starting processes" being the watchdog reset: the radio restarts.

That also explains Roy's blank decode list. IQ mode is session state on the QMX,
so a restart clears it, and the USB link usually survives a QMX restart with no
disconnect the Tab5 can see. What is left is a radio that answers every CAT
command and sends no audio: transmit works, nothing decodes, the list ages out.
Only a manual restart cleared it, because that forced a fresh handshake.

The next build re-sends the IQ-mode enable after 30 seconds of silence with CAT
still answering, which should recover both cases unattended. The log line is
"re-asserting IQ mode" — tell me if decodes return after it.

Stan, RF gain on the Tab5 also removes Samuel's reason to open that menu
mid-session.

A question for you. You said any character over CAT locks the radio while it is
in the Tune SWR menu. The Tab5's Antenna Tune (1.04 only) enters SWR Tune with
MD8, then polls PC and SW continuously for the live power and SWR readout. Does
that apply to the mode entered over CAT, or only to the front-panel menu? If
both, I am polling into the hazard and will change it. It would also account for
a crash report I had months ago that followed use of the SWR Tune submenu.

The quick-menu idea is written down: overlaying a button you do not use with your
own choice of shortcuts.

## To Samuel W7STF ALONE — post in the Tab5 panadapter topic

This is the Bluetooth-mouse answer, which came from the panadapter topic rather
than the Band config thread. It ends by pointing at the other message so he does
not have to wonder where his relayed suggestions went.

Samuel — on the Bluetooth mouse: a USB mouse already works today, but only with
the radio unplugged. The Tab5 has one USB host port, and sharing it needs a hub,
which needs a feature (a transaction translator) that the ESP32-P4's USB stack
does not implement. So the mouse is currently for setup and log reading, not
operating.

Bluetooth is the right long-term answer for using one while operating, and the
Tab5's radio chip does support it. The honest reason it is not done: Bluetooth
runs through the same co-processor link as WiFi, which has been the most fragile
part of this whole system, and I am not ready to add a new subsystem to that link
while it is finally behaving. Deferred, not rejected.

Meanwhile, most of what a mouse would do from across the desk you can now do from
any browser on your WiFi — including replying to stations and tuning — which may
scratch some of the same itch.

And thank you for relaying Stan's two suggestions. Both are built: RF gain over
CAT, and a "Release radio" pause for getting at the QMX's menus. I have answered
those properly over in the Band config thread, along with what I think is going
on with the QMX+ reboot you reproduced there.

## To BD4AHS — power reading rise time

The slow rise you see is expected: the Tab5 polls the radio's power reading over
CAT about once a second while transmitting, and the QMX's own measurement settles
over the first seconds of a burst — so the display catches up a couple of seconds
behind the radio. Your observation that they agree after ~3 seconds is exactly
right, and nothing is wrong on either end.
