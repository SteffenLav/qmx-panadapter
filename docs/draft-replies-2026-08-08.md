# Draft replies, groups.io, 2026-08-08

Four replies. For the operator to review and post — nothing posts automatically.
(Samuel's Bluetooth-mouse answer is folded into the Stan/Band-config one, so he
gets a single message rather than two.)

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

## To Samuel W7STF and Stan KC7XE — the Band config thread

Samuel, Stan — both of Stan's suggestions are in the next build, and the Band
config behaviour now makes sense.

RF gain over CAT: done. The QMX exposes it as RG, and it turns out to be
available on 1.03 as well as 1.04, so nobody has to move firmware for it. It sits
directly under the volume slider in Settings, in dB, 0 to 99 with the radio's own
default at 54. Two things it does differently from the volume, both on purpose:
it is a *per band* value — the same figure the Band Configuration screen edits —
so the Tab5 reads it back from the radio rather than remembering a number that
would belong to whichever band you were on last, and it writes on release rather
than continuously while you drag, because unlike volume this is stored
configuration, not a session setting.

The pause button: also done, and it turned out to be more load-bearing than a
convenience. The QMX's own menu and its Terminal Applications talk over the same
USB serial port the Tab5 polls three times every 150 milliseconds, so anything
you do on the radio is being interrupted the whole time. "Release radio" in
Settings — or the button on the web page — stops all of it: no polling, and the
watchdogs that would otherwise interpret a deliberate menu visit as a fault stand
down too. A blue bar across the top says the radio is yours and how to take it
back, because with the spectrum frozen that state otherwise looks exactly like a
failure. Taking it back re-checks IQ mode, which the menu can switch off.

Which brings me to the Band config thread, where Stan had it worked out before I
arrived.

His reading matches everything I can see from this end. The front-panel menu
drives the display from the MCU and is not isolated from the CAT channel the way
the terminal menu is — which is exactly why PuTTY is fine, and why unplugging USB
is fine. And the blank screen followed by "Starting processes" being the
watchdog reset is the piece that makes the rest fall into place, because it means
the radio genuinely restarts.

That turns out to explain Roy's problem too, which I had been treating as
separate. IQ mode on the QMX is *session* state — it is not saved — so when the
watchdog restarts the firmware, IQ mode is switched off. Meanwhile the USB link
usually survives a QMX restart without the Tab5 seeing any disconnect at all. So
the Tab5 is left with a radio that answers every CAT command perfectly and sends
no audio: transmit works, nothing decodes, and the decode list ages out to blank.
That is precisely Roy's report, and it is why only restarting the radio appeared
to fix it — the restart forced a reconnection and a fresh handshake.

So the next build simply asks again. After 30 seconds of silence with CAT still
answering, it re-sends the IQ-mode enable, which should recover both of your
cases by itself without anyone touching anything. If it works you will see it in
the log as "re-asserting IQ mode" followed by decodes resuming. I would very much
like to know whether it does.

Stan — your pause button and your "RG should ideally be accessible via Tab5" are
both in the next build, which I had started before reading your message, so
consider that agreement rather than obedience. Between them, Samuel's reason for
being in that menu goes away: RF gain becomes a slider you can move mid-QSO
without opening anything on the radio.

One thing I would like your opinion on, because it worries me now that you have
said it. You mentioned that in the Tune SWR menu, *any* character over CAT locks
up the radio controls. The Tab5 has an Antenna Tune button for 1.04 firmware
that enters SWR Tune with MD8 and then polls PC and SW continuously to show live
power and SWR while it transmits. Does your warning apply to the mode when it is
entered over CAT, or only to the front-panel menu? If it applies to both, I am
polling straight into the hazard and I will change it — and it would also explain
a crash report I had months ago that arrived right after someone used the SWR
Tune submenu. I would rather ask than guess.

Your quick-menu idea is a good one and I have written it down: overlaying a
button you do not use (presets, in your case) with your own choice of shortcuts.
That is the right shape for it — configurable, not another fixed row of buttons.

On the Bluetooth mouse from your earlier note: a USB mouse already works today,
but only with the radio unplugged — the Tab5 has one USB host port, and sharing
it needs a hub, which needs a feature (a transaction translator) the ESP32-P4's
USB stack does not implement. So the mouse is currently for setup and log
reading, not operating. Bluetooth is the right long-term answer and the Tab5's
radio chip supports it; the honest reason it is not done is that Bluetooth runs
through the same co-processor link as WiFi, which has been the most fragile part
of this system, and I am not ready to add a subsystem to that link while it is
finally behaving. Deferred, not rejected. Meanwhile most of what a mouse would do
from across the desk you can now do from any browser on your WiFi, including
replying to stations and tuning.

## To BD4AHS — power reading rise time

The slow rise you see is expected: the Tab5 polls the radio's power reading over
CAT about once a second while transmitting, and the QMX's own measurement settles
over the first seconds of a burst — so the display catches up a couple of seconds
behind the radio. Your observation that they agree after ~3 seconds is exactly
right, and nothing is wrong on either end.
