# Draft replies — 2026-08-19

Not sent. One post per person. Plain text, ready to paste.

Accuracy notes for me, not for posting:
- Verified on hardware: the terminal colours (both screens, pixel-measured), the
  web-UI stall fix on the server side (0 teardowns, 0 ECONNRESET, 8 partial writes
  healed in 9.3 min).
- Fixed but NOT reproduced here: the battery flapping (this unit has a pack fitted),
  the Cloudlog subnet rule (no Cloudlog server), the second-serial-port retry (this
  radio has two ports).
- NOT reproduced at all: the blank first entry into Radio Menus. 7 opens out of 7
  were clean, including the first after a flash.
- Untested against a real server: the whole Cloudlog HTTP conversation.

---

## 1 — Samuel W7STF

Samuel, the web-UI stall is real and I found it. Thanks for keeping on about it,
because it was not what either of us assumed.

I measured before changing anything. Over 9.6 hours the browser session was torn
down 545 times, roughly every 14 seconds in bursts, and each teardown cost about
2.2 seconds of frozen display while the browser reconnected. Worst case 4.5
seconds. Between the drops the stream ran at its full 10 frames per second. That
matches what you described exactly.

You guessed background processing. Half right. Feed traffic does trigger it, and
that is why it got worse with every release, because I keep adding feeds. But the
fault is a bug in Espressif's WebSocket send. It writes the frame with one call
and only checks for a negative result, and a partial write is not negative. So a
frame that went out half-finished was reported as sent. Your browser then read the
next frame's header as the tail of the previous one, decided the stream was
corrupt, and closed the connection. That is why it freezes for seconds instead of
just glitching one frame.

Worse, my own fix from July made it more likely. I put a 400 ms send timeout on
that socket to stop a stuck send freezing the whole web server, and a send timeout
is exactly how a half-written frame happens.

Fixed in v1.8.7. The send now loops until every byte is out, and if it cannot
finish a frame it closes the session rather than leaving your browser reading
rubbish. Please tell me whether the stalls stop. The device counts the repairs, so
if you send me a diagnostic log I can see it working.

Radio menus in white. Fixed. The radio does send colour and I was storing it, but
both my screens threw it away. Your version numbers at the bottom come out yellow
now, on the Tab5 and in the browser.

Blank screen the first time into Radio Menus after a flash. I cannot reproduce it.
I tried the first open after flashing plus six more, and all seven came up clean.
Randy sees the same thing in PuTTY, which points at the radio redrawing rather
than at me. If it happens again, tell me your QMX firmware version and whether
PuTTY does it at the same moment.

The GPS and serial ports warning you saw once after swapping cables. That was my
bug and it is fixed. I only tried to open the second port once, and any failure
printed that message. Swapping a cable makes the radio re-enumerate, so it was not
ready yet and I told you to go change a setting that was already correct. It now
retries, and it only blames the setting if the radio is definitely there.

73 de Stef OZ1LAV

---

## 2 — Randy N4OPI

Randy, answers to all three.

Long term plans for FT8 web access. The Tab5 screen will not be mirrored to the
web. That is a decision I took a while ago and it is not going to change, so please
do send your list rather than waiting.

The reason is the hardware. Decoding FT8 saturates one of the two cores, which is
why the spectrum and waterfall stream is switched off while the Tab5 is in FT8
mode. Mirroring the screen would be paid for in decodes, and decoding well is the
whole point of the thing. What I am happy to add is controls, which is where the
web page is genuinely thin today. It cannot touch the filters or auto-answer, edit
your CQ presets, send the mid-QSO override buttons, or set the clock. Those are all
things you would want from another room, so they are fair game.

Battery reading 100 percent then 0 percent with no pack fitted. Fixed. I had a
detector for the missing battery already, and it was flapping in time with the
rail instead of latching. It watched a five sample window, so while the voltage sat
at one value the window looked perfectly steady and the detector decided a battery
was present again. Now it also checks two things a real pack cannot do, which is
jumping several volts between readings, and running the device at 4.2 volts. The
web page also gets told, so it shows USB instead of inventing a percentage.

I could not reproduce it here because this unit has a battery in it, so tell me
whether it behaves.

Also fixed since we last spoke. Waking from the screensaver no longer acts on the
tap that woke it. And the spot and frequency labels on the web page are readable
again, they were being drawn at half size on a high resolution display.

73 de Stef OZ1LAV

---

## 3 — Roy KI0ER

Roy, two things.

Thank you for checking the CW waterfall against a real signal. Within 5 Hz at 650
is good enough for me to drop the warning I had put in the release notes saying it
was unverified.

The tuning dance you saw, plus 10 Hz then another 25 then back, is the spur
suppression working. It is off by default, so you have turned it on. It nudges the
dial 25 Hz and measures again, because a spur made inside the radio moves 16 to 50
times as far as the dial does while a real signal moves with it. That is how it
tells them apart. It caches the answer per frequency, which is why it settles down
and does not repeat when you come back to the same spot. Turn the setting off and
the nudging stops.

73 de Stef OZ1LAV

---

## 4 — Markus DL8MBY

Use docs/reply-dl8mby-vfo.md as written. Nothing has changed, and the fix shipped
in v1.8.7.

---

## 5 — Mark G4MEM (GitHub issue: Cloudlog)

Mark, yes. Your suggestion is the right one and I have built it.

Plain http is allowed when the server is on the same subnet the Tab5 is currently
connected to. Anything else needs https with a certificate. That is not the same as
an ignore the certificate switch, which I was not willing to ship, because the
subnet test is a fact I can check rather than a promise the operator makes. On your
own network the packets never leave equipment you own.

Two details worth knowing before you try it.

The check runs on every upload, not once when you save the address. That matters
for exactly your use case. You set it up at home, then operate from a field site,
and away from home the same hostname could be answered by anything. So the upload
simply refuses until you are back on your own network, which is what you asked for
anyway.

For plain http the address has to be numeric, so http://192.168.1.20 rather than a
name. A name has to be looked up, and what answers the lookup can differ from what
answers when the connection is made, so there would be nothing solid to check. Use
https if you want to use a hostname.

It sends in batches, and Cloudlog does its own duplicate checking, so re-uploading
costs time and nothing else. Wavelog works too, since the address is yours and the
API path is the same.

I have no Cloudlog server here, so the subnet rule is tested but the conversation
with your server is not. You are the first person to run it. Tell me what happens.

73 de Stef OZ1LAV

---

## 6 — Brian WA6JFK

No reply needed. He accepted the answer and agreed with the behaviour.
