# Groups replies — everything fixed for v1.8.4 (drafts, NOT yet posted)

Plain text, one reply per person. Copy the block under each name. Nothing below
is formatted for markdown.

**v1.8.4 is NOT released yet.** No dates are promised anywhere in here. If you
post these before the release, they read as "it is fixed and coming"; if you post
them alongside it, add the download line yourself.

⚠ **Six of the ten fixes have NOT been tested on hardware** — nothing here has
touched a real antenna. Each block asks the person who reported it to check the
specific thing they reported, which is both honest and the fastest way to get
them tested. Do not let me quietly upgrade those to "done".

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

---

## 3. Samuel W7STF

On flashing failing while the COM port was still listed: your suggestion is now written into the instructions - "reboot the Tab5 and try again" is the first troubleshooting step, in the flasher's own README, the quick start, the troubleshooting page and the main README.

On why it happens: the Tab5 has no separate USB-to-serial chip. The port you flash over is produced by the ESP32-P4 itself, so it only exists while the firmware is running - a busy firmware can leave the port listed in Device Manager while it no longer answers the flasher. Restarting it restarts the port. I have not endorsed the memory-depletion idea, because nothing has measured it, and I would rather leave the cause open than write a guess into the manual.

On the arrow buttons: understood, and thank you for closing that off. The proportional slider stays as it is.

---

## 4. Brian WA6JFK

On the config file that would not take: there was a real gap behind that. If nothing in an uploaded file is recognised, the page said "Config applied: 0 item(s)" - which reads exactly like success. It now says plainly that nothing was recognised, lists the likely causes, and tells you that nothing on the Tab5 was touched, so the next step is to fix the file rather than reset the unit.

On the erase install: it worked, and there was no harm in it - but you should not have needed it, and the "Change QRZ API key" and "Change eQSL login" rows in the next release mean you will not again.

On the POTA activation: 7 FT8 contacts from a portable vertical after 30 on SSB is a good afternoon, and thank you for the photo.
