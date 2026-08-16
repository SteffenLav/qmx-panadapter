# Groups replies — everything fixed for v1.8.4 (drafts, NOT yet posted)

Plain text, one reply per person. Copy the block under each name. Nothing below
is formatted for markdown.

**v1.8.4 is NOT released yet.** No dates are promised anywhere in here. If you
post these before the release, they read as "it is fixed and coming"; if you post
them alongside it, add the download line yourself.

⚠ **Read this before posting — what is and is not verified.**

Much of the 16 Aug work IS hardware-verified now that the antenna is back: the
TX-offset fix, all four auto-answer safety items, the terminal, the spur
measurements, and the top-bar label fix were all checked on the radio.

Still NOT verified, and the replies say so in each case:
 - **Kevin's mouse** — fixed against what his symptom implies; I have no Surface
   Arc Mouse. His reply asks for a diagnostic log, which settles it in one line.
 - **Gyula's Close button and the failed-write guard** — the colour has not been
   eyeballed, and the write failure needs a deliberately full filesystem.
 - **Don's Pile Up changes** — still need a finger and a real pileup.
 - **Roy's phantom CW** — my explanation was FALSIFIED on hardware tonight. His
   reply says so outright. Do not let it be posted as fixed.

Do not let me quietly upgrade any of those to "done".

---

## 1. Don WB0LQW — READ THE CLOCK PART, it affects you NOW

Congratulations on the activation - 22 contacts on your first FT8 outing, and the ADIF accepted by POTA, is a very good day. "I forgot the QMX was even there" is the nicest thing anyone has said about this project.

On the clock, and please read this one before your next activation: the seconds box does NOT lock when you tap it. Tapping it changes the clock SOURCE, cycling FT8, then NTP, then your QMX. So the two taps you were doing to get the grey border were selecting the QMX's own clock as your time reference. On a QMX without GPS that is the least trustworthy source there is - it starts at 00:00 when you switch the radio on - and it is the exact thing v1.8.2 stopped doing automatically. Until the next release, leave the seconds box alone: it is already correct with the blue border. Nothing you did caused any harm, and your contacts are fine.

On why you thought otherwise: because the screen told you so. The hint under the box said "SS NTP sync" and never said tapping changed anything, and the comment in my own source described a lock that stopped existing some releases ago. That is my fault, not a misreading on your part.

On the fix: the hints now say "NTP source (tap)" and "QMX source (tap)", so it is obvious that tapping changes the source. And the FT8 line now shows the running total of the correction applied, not just the last nudge - which is the "how much total correction was applied" you asked for. The total was always being tracked internally; it was simply never shown.

On what I have not fixed: "how long should I let it nudge". You are right that there is nothing telling you when it has settled, and a number that keeps twitching gives you no way to judge. I would rather think about that properly than add a label that guesses.

On the Pile Up screen: you were not being clumsy. The dismiss X was small, and missing it hit the row underneath - and tapping the row WORKS that station, which opens the transmit ladder and closes the screen. That is why you kept ending up out of the screen. The X is now bigger and there is a dead zone beside it that absorbs a near miss and does nothing at all, so the worst case is now that nothing happens instead of you calling someone you did not choose.

On clearing the Pile Up list: added. There is a "Clear all" button beside Close, and it asks once - the first tap changes it to "Clear 5?" and the second clears it. You were right that a long list goes stale faster than it can be worked.

On seeing your QSO count without fighting the Pile Up screen: the QSO log page already shows the count, and the Pile Up screen no longer traps you. If that still feels awkward in use, tell me - the underlying want was "how many have I got", and there may be a better place to put that than either screen.

On the learning curve, and knowing when to bail out on a contact: that is worth saying out loud, because the panadapter already bails out for you and it does not tell you that it does. If a station stops answering, the exchange is abandoned after four slots with no progress and the radio goes back to what it was doing. There is also a grey-list, off by default, which skips a station that has timed out on you twice, so a silent station does not keep eating your cycles. What is missing is any of that being visible while it happens - you were tracking it in your head because the screen was not tracking it for you. I have not got a good answer yet, but it is the right complaint and I would rather say so than dress it up.

Two of the above - the Pile Up changes - I have not been able to test, because the screen needs a finger and a real pileup. If you get a chance on your next activation, they are the two to watch.

---

## 2. Roy KI0ER

On the second station being sent your GRID instead of a report: found, fixed, and it was exactly as you described. This was the important one in your three messages - it was losing you contacts.

On the cause: when a QSO finishes and there are callers waiting, the panadapter picks the next one up automatically. That path was building the message as if IT were calling THEM - which starts with your grid. But everyone in that pileup called YOU, so they already have your grid and their software is waiting for a signal report. Getting a grid instead, they give up, which is precisely what you saw. The odd part is that doing the same thing by hand from the pileup window was always correct; only the automatic pick-up was wrong, which is why it took your report to find it.

On testing it: I ran a full CQ run in simulation and watched four stations answer. The first is handled by different code and was always right; the three worked automatically from the pileup now all get a report - VK3ABC +09, W1AW +08, N5XYZ +08 - instead of a grid. It has not been on air yet, so if you run a CQ and draw a pileup, that is the thing to watch.

On the CQ run stopping after the pileup drains: it did, and it should not have. Draining the pileup was quietly ending the CQ session, so once the queue emptied the radio just stopped calling - mid-activation, while you would reasonably believe it was still running. Fixed: nothing automatic ends a CQ run now. Only you do, or the "stop after N calls" setting if you have set one.

On the strip turning all green in the window you are transmitting into: fixed, and you were right about which way it should go. The problem was that "I heard nothing in that window" and "that window is empty" looked identical, and both were drawn green - which is the one colour that will send someone onto an occupied frequency. It now goes grey, the same as before anything has been heard. It greys after about four slots of not hearing that window, so a single missed cycle does not make it flicker.

On listening to the rest of a slot after you cancel a transmission: done. If you cancel early, the remainder of that slot is now received normally and feeds the map, instead of the slot being written off. It turned out to need no new machinery at all - the audio was already being kept continuously for other reasons, so the ordinary receive path could just pick the slot up.

On long-pressing a CQ to pounce without the modal: done. Hold the row for about three quarters of a second and release, and it pounces immediately - no confirmation window. It only applies to plain CQ rows, and releasing after sliding off the row cancels it, so it cannot fire by accident while you are dragging the highlight around.

On RIT - all three of your points were right:
 - There was no way to clear a PARKED offset. Long press parked it, long press again brought it back, and a short press only armed the tap-to-set mode. A short press now discards a parked offset, which is what "off and zero" should mean.
 - A band change genuinely did not clear it. It cleared an ENGAGED offset but not a PARKED one, so a parked offset survived onto the next band and came back on the next long press - from a number you could no longer see. Now any retune drops it.
 - RIT and the CW transmit offset are now mutually exclusive, as you said they should be. The CW offset works by splitting the VFOs, since the QMX has no XIT, so allowing RIT on top would leave you listening on one frequency, transmitting on a second and reading a dial showing a third. It now refuses, and says why.

On the RIT changes: none of them have been tested on a radio - they need CW and a finger. If you have five minutes with them, you found all three, so you are the right person to say whether they now behave.

On the kind words: thank you. Four of the six things in this release came from you.

### Roy — added 16 Aug (your TX-offset post, the auto-answer batch, and the phantom-CW email)

On picking a new TX offset during a QSO and it being ignored: fixed, and your reasoning is the rule I coded to. Nothing in FT8 requires either station to stay on the offset it started on - only to stay in its time window - so there was never a reason to remember the old one.

On what was actually wrong, because it was not what it looked like: the offset change itself always worked. What did not work was WHEN you were allowed to make it. A transmission occupies about 12.6 seconds of a 15-second slot, and in a QSO you transmit every other slot, so roughly four attempts in ten landed mid-burst and were refused outright with "try again after this burst". A refused change leaves the exchange running on its original offset - exactly what you described. Your choice was depending on your timing, which is not a choice.

It now accepts the new offset immediately and applies it the moment the burst ends. The burst in progress still finishes on the old offset, because stopping mid-transmission would send a corrupted frame, but the very next message carries your new one. Verified here: a QSO running at 1650 Hz, changed to 2450 mid-burst, next message out at 2450.

On the four auto-answer safety points: all four are done, and all four are verified on hardware.
 - It no longer transmits before it has mapped both time windows - it waits, and says "listening before first call".
 - Cancelling a transmission now switches auto-answer off as well. One tap on the TX indicator disarms, aborts the QSO and stops the automatics, so halting a transmission to go and check your antenna does what you expect.
 - A band change switches it off. This one was genuinely broken - it only worked from the band buttons, so changing band from the web page, a spot, a memory recall or the radio's own knob left it running into an antenna that was probably not tuned. That is the route you would most likely have hit.
 - It is off at every startup, however the last session ended.

On your phantom-CW email and the two logs: thank you, and the reproduction recipe is the most useful thing anyone has sent me on this. Being able to cause it deliberately is worth more than any number of sightings.

I have to be straight with you though: I had a theory that fitted everything you described, I tested it here tonight, and it is wrong. The theory was that visiting the radio's front-panel menu drops the QMX out of I/Q mode, which would fold the two sidebands together and put a mirror copy of every signal on the wrong side of centre - and that "Let me use the QMX menus" then "Done" cured it because that hands I/Q mode back. I set the radio up exactly as your log showed yours (CW, 14.061, the CW transmit offset engaged at 50 Hz), read the I/Q state, changed the sidetone volume from the front panel as you did, and read it again. It was still on. So that is not the mechanism, and I am not going to ship a fix built on it.

What your log did tell me: the menu visit leaves no trace at all in the panadapter's log, and the audio keeps flowing at full rate throughout - so whatever changes, the radio is still streaming and the Tab5 still thinks all is well. That narrows it usefully.

One thing that would help more than anything: the screenshot you posted shows it clearly, but if you can catch it again, note whether the phantom is the same distance to the LEFT of the real signal as the real signal is from the centre line. If it is, it is a mirror and the cause is in the I/Q path; if it is not, it is something else entirely, and I have been looking in the wrong place.

---

## 3. Samuel W7STF

On flashing failing while the COM port was still listed: your suggestion is now written into the instructions - "reboot the Tab5 and try again" is the first troubleshooting step, in the flasher's own README, the quick start, the troubleshooting page and the main README.

On why it happens: the Tab5 has no separate USB-to-serial chip. The port you flash over is produced by the ESP32-P4 itself, so it only exists while the firmware is running - a busy firmware can leave the port listed in Device Manager while it no longer answers the flasher. Restarting it restarts the port. I have not endorsed the memory-depletion idea, because nothing has measured it, and I would rather leave the cause open than write a guess into the manual.

On the arrow buttons: understood, and thank you for closing that off. The proportional slider stays as it is.

On leaving it running seven hours and coming back to it still going: that is worth more to me than it probably felt like to report. Long unattended runs are the thing I cannot test properly here, so an "it was still motoring along" from a real station is real evidence.

### Samuel — added 16 Aug (spur suppression, and the terminal)

On spur suppression not seeming effective: you were right, and the reason is not the one either of us expected. I measured it here before changing anything, because the alternative was redesigning a detector on the strength of an impression.

The detection is not the problem. On 20m it found 87 spur bins, the strongest 38.5 dB over the noise floor at exactly -8015.6 Hz with its second harmonic where it should be. It finds them precisely.

The problem was that the menu offered the weaker treatment first. There are two: "Subtract spur power" removes the measured power and can never hide a real signal, and "Erase spur bins" replaces them. Measured on the waterfall, at the spur columns: subtract takes them down about 28 per cent, erase takes them down about 78 per cent, and both remove the saturated red cores. So "not all that effective" is a fair and accurate description of subtract - which is what the list put in front of you.

"Erase spur bins" now comes first. Two things about it turned out better than I expected, both measured rather than argued: it leaves no dark notches, because it ramps between the neighbouring bins instead of blanking them; and the measurement is cached per frequency, so the roughly three seconds it spends nudging the dial happens once on a frequency you have not visited, and coming back to it later is free.

The three seconds on a new frequency is not going away, and it is worth knowing why. A spur's offset moves 16 to 50 times faster than the dial does. That is exactly what makes it detectable - nudge the dial 25 Hz and a spur jumps 8 to 27 bins while a real signal moves half of one - and it is also why the map is genuinely invalid after even a 100 Hz retune. You cannot have the detector without the cost.

If it still underwhelms you on erase, tell me and send a before/after screenshot - that would be a different problem from the one I just fixed, and I would want to see it.

On the terminal you and Michael were discussing: it is built, and it is in this release. It sits on the QMX's SECOND USB serial port - the radio can expose two, it is a System config setting - so it does not fight the CAT link at all; the panadapter keeps decoding while you are in the radio's menus. Your instinct that it might not fit was reasonable, but Michael was right: the escape-sequence handling is a small amount of code, and the whole thing is about 300 lines plus a font.

---

## 4. Brian WA6JFK

On the config file that would not take: there was a real gap behind that. If nothing in an uploaded file is recognised, the page said "Config applied: 0 item(s)" - which reads exactly like success. It now says plainly that nothing was recognised, lists the likely causes, and tells you that nothing on the Tab5 was touched, so the next step is to fix the file rather than reset the unit.

On the erase install: it worked, and there was no harm in it - but you should not have needed it, and the "Change QRZ API key" and "Change eQSL login" rows in the next release mean you will not again.

On the POTA activation: 7 FT8 contacts from a portable vertical after 30 on SSB is a good afternoon, and thank you for the photo.

---

## 5. Randy N4OPI and Michael KZ4LY — the terminal is built

You asked for a terminal emulator so a headless QMX+ could be configured without a laptop. It is in this release.

Where it is: on the Tab5, Settings drawer, Radio, "Radio menus". In the browser, the Radio menu at the bottom. Both show the radio's own 80x24 menu screen, with arrow keys, Enter and Back.

One thing to do first: the QMX has to be told to offer a second USB serial port - System config, GPS and Ser. ports, USB serial ports, set to 2. It is a one-off and it survives a power cycle. Randy, on a headless unit you can set that over CAT rather than needing the panel.

Why the second port matters, since it is the whole design: the QMX manual is explicit that leaving a terminal session without choosing "Exit terminal" leaves the radio refusing CAT commands. On the port the panadapter already uses, that would take the whole display down. On its own port it cannot - and I measured it, CAT frequency, mode and the S-meter all kept running for the length of a session. Closing walks the radio back out through its own "Exit terminal" item, and it finds that item by reading the screen rather than counting keypresses, so it works however deep in the menus you are. If the browser tab is closed, or nothing happens for two minutes, it hands the radio back by itself.

Michael, on your point about it not being as big as feared: you were right, and it was worth saying. The escape-sequence handling is a short piece of code, and the QMX only uses a handful of sequences. The one thing that turned out to be load-bearing was reverse video - it is the ONLY thing marking which menu item is selected, so a renderer that dropped it would leave you unable to see where you were.

Randy, on your 3D-printed enclosure at 12 oz with the QLG3 in it: that is a genuinely impressive piece of packaging, and it is the use case this feature exists for.

---

## 6. Kevin KW6E — the Surface Arc Mouse

On the pointer moving oddly, fast sideways and barely at all vertically: found and fixed, and your description is what identified it. That specific asymmetry can only be one thing.

Your mouse reports its movement as 16-bit numbers. The panadapter was reading them as 8-bit, so the horizontal value it used was the bottom half of the real one - which wraps round every 256 counts, hence the jumping between points - and the vertical value it used was the TOP half of the horizontal one, which is almost always zero. Hence fast horizontally and nearly dead vertically.

On why: mice are allowed to describe their own report layout, and the panadapter was assuming the simple layout that the older "boot protocol" mice use, without checking. It now reads the description your mouse publishes and decodes accordingly.

On the speed and acceleration setting you asked for: I have deliberately NOT added one, because it would have hidden this rather than fixed it - no amount of scaling makes one axis wrap round while the other stands still. If the pointer speed still does not suit you once it is tracking properly, say so and I will add a proper setting for it.

I should be honest that I do not own a Surface Arc Mouse, so this is fixed against what your symptom implies rather than against your actual hardware. If it is still wrong, send me a diagnostic log from just after you plug it in - there is now a line in there describing exactly how the panadapter read your mouse's layout, and that will tell me in one line whether I got it right.

---

## 7. Gyula HA3HZ — first-use notes

Thank you for writing these up while they were still fresh. Notes from the first few days are the ones that find things regular users have stopped noticing.

On the Close button in the QSO log being red like Delete all: fixed, and it was worse than you described - Close was the BRIGHTER red of the two. Red is the colour that means "cancel" everywhere else in the interface, which is harmless where the worst outcome is that nothing happens, but that window is the one place with a destructive button in it. Close is now neutral, so in the log window red means one thing only: it deletes your log. You were being more careful than the interface deserved.

On how much the log holds: about a couple of thousand contacts. The internal storage is 1 MB shared between the log, the diagnostic log and the LoTW certificate, which leaves a few hundred KB for contacts. Download it and clear it now and then rather than letting it run indefinitely.

And your question found a real bug, which is why it was worth asking. If that storage ever did fill up, the panadapter wrote the contact, failed silently, and still told you it had logged it - you would have found out at upload time. It now checks, refuses to count a contact it could not save, and tells you on screen which station it was.

On the ADIF file having no daily breakdown: correct, it is one continuous file. The viewer on the Tab5 has a Today/All filter, so you can see today's contacts there, but the downloaded file is everything. If a per-day export would help for POTA, say so - that is a reasonable thing to want.

On the pile-up, two stations answering and only the first being worked: what you observed is right, and the gap is that nothing on screen tells you what is happening. The panadapter works them one at a time; if the second is still calling when the first finishes, it picks them up automatically. If it has gone quiet, calling it repeats a few times and then gives up - which is the "reports x times and times out" you saw. Neither of those is wrong, but neither is visible, and a station that answered ninety seconds ago may genuinely have left. I have not fixed that, because I do not yet have a good answer for what the screen should show. It is written down, and you are the second person to bump into it.

On learning it being harder than building it: that is fair, and it is the honest state of things. Everything is documented, but knowing which of forty controls matters on your first evening is a different problem from documenting all forty.

---

## 8. Tony Abbey — the battery (email + list thread)

Roy and Samuel have already given you the right answer, and I want to confirm it from my side: this is not the panadapter.

The Tab5's power button is a soft shutdown, not a switch in the battery line, so a small drain continues after it appears off. M5Stack also document that it can only charge once it is powered on and initialised - so plugging in USB while it is "off" does not offset that drain. And if the pack drops below about 6 V it goes into protection, which needs an external charger to recover; the Tab5 cannot bring it back on its own.

Practical advice, which is Roy's: slide the NP-F550 off if you are storing it for more than a few days, and if the pack is already flat you will need a charger made for that battery type to wake it up.

I will add this to the manual, because you will not be the last person to meet it. If you do see something that looks like the firmware draining it - for instance it discharging noticeably faster on one release than another - tell me, because that WOULD be mine and I would want to know.

---

## 9. Michael KZ4LY — your crash log (email reply)

Thank you for sending it even though you could not reproduce it, and for saying so plainly. One-off reports with a log attached are worth more than they feel like.

I could not diagnose it, and the reason is my fault rather than yours: the log you sent CANNOT contain the crash. The microSD copy only receives the first few seconds of each boot when WiFi is on - that was a deliberate decision to stop the card being written to constantly while WiFi is up, because writing to the card and running WiFi at the same time is what used to wedge the network on this board. The consequence, which I had not thought through, is that the file is seventeen boot headers each about five seconds long. It stops before anything interesting happens, every time.

That is worse than having no log, because it looks like evidence. I have written it down as something to fix.

What I could tell from it: no crash backtrace anywhere in the file, and the resets are recorded as watchdog resets rather than software panics. Combined with the light blue screen you saw, that points at something holding the processor's interrupts off long enough for the hardware watchdog to fire - the light blue is the display giving up on a frame, which is a symptom this project has chased before and which has always turned out to be something blocking for too long rather than the display itself.

If it happens again, the file to send is qmx-log-saved.txt, from the web page's "Diagnostic download" - that one is kept in the Tab5's internal flash, it keeps appending, and it survives a restart. That will have the moments before the crash in it. A serial capture would be even better if you happen to be at a computer, but the saved log is the easy one.

On the 180-degree rotation: noted, and worth mentioning again if it recurs. I have no reason yet to think it is involved, but you noticed it was your first time using it and that is exactly the kind of detail that turns out to matter.

On your handheld all-in-one: the terminal in this release should help that plan along - see the note above. A QMX+ with no panel is now configurable from the Tab5 itself.
