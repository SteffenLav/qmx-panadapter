# v1.8.7 announcement — draft

Plain text for groups.io. NOT posted.

---

**QMX Panadapter v1.8.7 is out.**

https://github.com/SteffenLav/qmx-panadapter/releases/tag/v1.8.7

Almost everything in this one came from someone telling me what was wrong.

**The browser panadapter stops freezing** (Samuel W7STF). He kept saying the web
display hung for seconds at a time and that it was getting worse with every release.
He was right on both counts, and it was not what either of us assumed.

I measured before changing anything. Over 9.6 hours the browser session was being
torn down **545 times** — roughly every 14 seconds in bursts — and each one cost
about **2.2 seconds** of frozen display while the browser reconnected. In between,
the stream ran at its full speed. That is why it felt like stalling rather than
slowness.

The cause is a bug in the WebSocket send underneath: it writes the frame with one
call and only treats a negative result as an error, and a partial write is not
negative. So a frame that went out half finished was reported as sent, your browser
read the next frame's header as the tail of the previous one, decided the stream was
corrupt, and hung up. The background feeds only made it more likely — which is
exactly why it got worse as I added more of them. Worse still, my own fix in July for
a different freeze made it more likely again: it put a timeout on that socket, and a
timeout is precisely how a half-written frame happens.

Please tell me whether the stalls stop. The device counts the repairs now, so a
diagnostic log will show it working.

**Upload to your own Cloudlog or Wavelog** (Mark G4MEM). The fourth logbook, and the
only one you host yourself. Plain http:// is allowed when the server is on the same
network as the Tab5, so a home server needs no certificate — but the check is made
at every upload, not once at setup, so away from home it refuses rather than sending
your API key across a network you do not control. That is deliberate: Mark's whole
use case is operating from a field site and uploading when he gets home. Use https://
for anything remote. Wavelog works too. If your server is on your own network this
is the only upload that needs no internet at all, which suits POTA and SOTA better
than the other three.

**Pick callers myself** (Eric K3FNB). He asked for the option to be more engaged when
activating a park: to tap the hunter he wants instead of the firmware answering the
first one. It is a checkbox in the FT8 Filter modal. With it on, a station answering
your CQ waits in the pile-up until you tap them, and the exchange then runs itself
as usual — only the choice becomes manual. It keeps calling CQ while you decide, so
the pile-up carries on building. Off unless you turn it on.

**Radio menus show the radio's own colours** (Samuel W7STF). They rendered white
while PuTTY showed the same screens in red and green. The colour was being read from
the radio all along and both my screens were throwing it away.

**A Bluetooth mouse whose pointer moved erratically** (Kevin KW6E). It connected and
scrolled perfectly but the pointer jumped about. Two separate things: the mouse fix
in v1.8.4 went into the USB code and his mouse is Bluetooth, so it never applied to
him at all; and the movement itself was being read with the wrong layout, turning a
small movement into a large jump the wrong way. Diagnosed entirely from the
diagnostic log he attached — thank you, that is exactly what makes this possible.

**Entering FT8 could reboot the device.** Caught on my own bench while testing this
release. A shared decoder buffer could be released twice if the decoder was set up
twice without being torn down in between. It is almost certainly the unexplained heap
crash that has been on my list since v1.3.0, and it now has a proper explanation
rather than a shrug.

**Also fixed:** the battery no longer reads 100% then 0% when no pack is fitted
(Randy N4OPI); waking from the screensaver no longer acts on the tap that woke it
(Randy N4OPI); the browser's spot and frequency labels are readable again on a
high-resolution display (Randy N4OPI); a radio left receiving on VFO B is put back
on A and tells you it did (Markus DL8MBY); a warning that told you to change a radio
setting that was already correct after a cable swap (Samuel W7STF); a crash after
about seven hours of healthy operation; and the flash-persisted diagnostic log no
longer stops writing with space still free.

**Investigated and not reproduced:** the first entry into Radio menus after a flash
drawing blank (Samuel W7STF). I tried the first open after flashing plus six more and
all seven were clean. Randy sees the same thing in PuTTY, which points at the radio's
own redraw rather than at the Tab5. If it happens to you, tell me your QMX firmware
version and whether PuTTY does it at the same moment.

**Two things I have not been able to verify myself**, so please shout if they
misbehave: the Bluetooth mouse fix (I do not own a Surface Arc — it is verified
against Kevin's captured data, not his hardware), and "Pick callers myself" on the
air (it is verified in simulation, both with the option on and off, but has not run
a real activation).

Full detail, including the numbers behind the WebSocket fix:
https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md

73 de Stef OZ1LAV
