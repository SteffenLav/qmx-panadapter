# Draft replies, groups.io, 2026-08-08

Three replies. For the operator to review and post - nothing posts automatically.

---

## To Roy KI0ER (the whole evening's batch)

Roy - that was a productive evening on your end. Point by point.

The dual EVEN/ODD strips are implemented, exactly as you argued them. Every
occupancy strip - the mini strip on the FT8 pane, the full picker, and the web
page's picker - now shows two rows, EVEN above ODD, always, never a combined
"BOTH". Your white marker sits on your own window's row once a transmission has
fixed it, and on both rows before that, because until then the tone is chosen but
the window is not. The picker's verdict says where you stand in words: "Clear in
EVEN - busy in ODD".

You also found a real bug in the same mail. The "FREQ BUSY" warning never
received the time-window filter the occupancy map got in v1.3.4 - it was still
counting stations in the opposite window, which cannot collide with you. That is
exactly why it contradicted the green strip. Fixed: it now judges only your own
window.

The auto-answer change is in too, on your reasoning verbatim: a robot pick that
turns out to be mid-QSO with somebody else is abandoned on the spot and the robot
chooses another CQ caller. A deliberate pounce still holds and waits - a willing
pounce means you want that station. No grey-list strike for the busy one; busy is
not unresponsive.

The blank message area: your two reports point at the same fault, and your QMX
menu reproduction was the giveaway. The radio stops sending IQ audio (the menu
visit being one trigger) while CAT keeps answering, so transmit still works,
nothing decodes, and the rows age out after a minute - a blank list with signals
audible. The next build watches for exactly that signature - audio at zero while
CAT is alive - and recovers by itself: a soft audio reset at 60 seconds, then the
same USB power-cycle you perform by hand, capped so a genuinely dead radio cannot
loop it. Your diagnostic logs would still be very welcome to confirm the
signature matches - please email them.

Your CQ-pause idea (skip an occasional transmission during a long CQ run so your
own window's picture refreshes) is a good one and is on the list - it changes
on-air conduct, so I want to think about the cadence rather than rush it.

And thanks for the v1.04.005 note and the grey-list confirmation. 1,100 miles
per Watt through 100 feet of coax at 2:1 is not bad at all for a "CW operator".

## To Samuel W7STF (Bluetooth mouse)

Samuel - no jitters, it is a fair question with a concrete answer.

A USB mouse already works today, but only with the radio unplugged: the Tab5 has
one USB host port, and sharing it needs a hub, which needs a feature (a
transaction translator) the ESP32-P4's USB stack does not implement. So the mouse
is currently for setup and log reading, not operating.

Bluetooth is the right long-term answer for mouse-while-operating, and the Tab5's
radio chip does support it. The honest reason it is not done: Bluetooth on this
board runs through the same co-processor link as WiFi, which has been the most
fragile part of the whole system, and I am not ready to add a new subsystem to
that link while it is finally behaving. It is deferred, not rejected.

Meanwhile, most of what a mouse would do from across the desk you can now do from
any browser on your WiFi - including replying to stations and tuning - which may
scratch some of the same itch.

## To BD4AHS (power reading rise time)

The slow rise you see is expected: the Tab5 polls the radio's power reading over
CAT about once a second while transmitting, and the QMX's own measurement settles
over the first seconds of a burst - so the display catches up a couple of seconds
behind the radio. Your observation that they agree after ~3 seconds is exactly
right, and nothing is wrong on either end.
