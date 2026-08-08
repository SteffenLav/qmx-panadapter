# Draft replies, groups.io, 2026-08-08

Five replies. For the operator to review and post — nothing posts automatically.

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

The blank message area: your two reports point at the same fault, and your QMX
menu reproduction was the giveaway. The radio stops sending IQ audio while CAT
keeps answering, so transmit still works, nothing decodes, and the rows age out
after a minute — a blank list with signals audible. Chasing that down led
somewhere useful: IQ mode on the QMX is *session* state, not a stored setting, so
a trip through the radio's menu can simply switch it off. The radio is then
working perfectly and just isn't being asked for IQ audio any more. So the first
thing the next build does, after 30 seconds of silence with CAT still answering,
is ask again — free, invisible, and it fixes that case outright. Only if silence
continues does it escalate to an audio reset at 60 seconds and the USB
power-cycle you perform by hand at 120, capped so a genuinely dead radio cannot
loop it. Your diagnostic logs would still be very welcome to confirm the
signature matches — please email them.

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

## To Samuel W7STF — Stan's two suggestions

Samuel — please pass both of these back to Stan, because both are in.

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

Which brings me to your other thread. I would like to see the Band config
behaviour you described on the QMX+ with fresh eyes, because I suspect it is this
same contention — the Band Configuration terminal app and our poll talking over
each other on one port. Could you try it once more on the next build with
"Release radio" engaged before you enter Band config, and send me a diagnostic
log either way? If it still misbehaves with the Tab5 silent, then it is something
else and I want to know that just as much.

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
