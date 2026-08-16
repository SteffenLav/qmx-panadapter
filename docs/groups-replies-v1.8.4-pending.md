# Groups replies — v1.8.4 (drafts, NOT yet posted)

Plain text, one block per person. Copy the block under each name.

⚠ **What is NOT verified** — each block says so where it applies. Do not let these
be upgraded to "done": Kevin's mouse (no Surface Arc here), Gyula's Close button
and the failed-write guard, Don's Pile Up changes, and **Roy's phantom CW, where
my explanation was falsified on hardware.**

---

## 1. Don WB0LQW

Congratulations on the activation — 22 FT8 contacts and POTA accepting the ADIF is a good day.

Read this bit before your next outing: tapping the seconds box does **not** lock it. It cycles the clock SOURCE — FT8, NTP, then your QMX. So your two taps were selecting the QMX's own clock, which on a non-GPS unit starts at 00:00 when you switch on. Leave it alone; it is already right with the blue border. Nothing you did caused harm.

You thought otherwise because the screen said so. The hint never mentioned tapping. In v1.8.4 it reads "NTP source (tap)" / "QMX source (tap)", and the FT8 line shows the running total correction — the number you asked for.

Pile Up screen: the dismiss X was too small, and missing it hit the row beneath, which works that station and closes the screen. In v1.8.4 the X is bigger with a dead zone beside it, so a near miss does nothing. There is also a "Clear all" button that asks once.

Not fixed: knowing when the clock has settled. You are right that there is nothing telling you. I would rather think about it than add a label that guesses.

The Pile Up changes need a finger and a real pileup — I could not test them. They are the two to watch.

---

## 2. Roy KI0ER

**Pileup sending your grid instead of a report** — fixed. When a QSO finished and callers were waiting, the automatic pick-up built the message as if it were calling them, which starts with your grid. But they called *you*, so their software was waiting for a report and gave up. Doing it by hand was always correct, which is why it took your report to find.

**RIT** — all three fixed: it clears to zero, a band change clears it, and it now refuses while the CW transmit offset is engaged (that works by splitting the VFOs, so allowing both would leave you listening on one frequency, transmitting on a second and reading a third).

**TX offset ignored mid-QSO** — fixed, and your rule is the one I coded to: nothing requires a station to stay on its starting offset, only in its time window.

The change itself always worked. What failed was *when* you were allowed to make it. A burst covers ~12.6 s of a 15 s slot and you transmit every other slot, so roughly 4 attempts in 10 landed mid-burst and were refused — leaving the QSO on its original offset. Your choice depended on your timing. In v1.8.4 it is accepted immediately and applied the instant the burst ends. Verified here: QSO at 1650 Hz, changed mid-burst, next message out at 2450.

**Auto-answer, all four, all verified on hardware:** it waits until both time windows are mapped before its first call; cancelling a transmission now also switches it off (one tap disarms, aborts and stops the automatics); a band change switches it off; and it is off at every startup. The band one was genuinely broken — it only worked from the band buttons, so the web page, a spot, a memory recall or the radio's own knob all left it running into an untuned antenna.

**Phantom CW** — thank you for the recipe and the logs. Being able to cause it deliberately is worth more than any number of sightings.

But I have to be straight: I had a theory that fitted everything, I tested it tonight, and it is wrong. I thought the front-panel menu was dropping the QMX out of I/Q mode, which would mirror every signal. I set my radio up exactly as your log showed yours — CW, 14.061, offset engaged — changed the sidetone volume from the panel, and read the I/Q state. Still on. So that is not it, and I will not ship a fix built on it.

Your log did narrow it: the menu visit leaves no trace at all, and audio keeps flowing at full rate throughout.

If you can catch it again, one thing would help most — is the phantom the same distance to the LEFT of the real signal as the real signal is from the centre line? If yes it is a mirror and the cause is in the I/Q path. If not, I have been looking in the wrong place.

---

## 3. Samuel W7STF

**Flashing while the COM port is still listed** — your suggestion is now the first troubleshooting step in the flasher README, quick start and troubleshooting page. The Tab5 has no separate serial chip: the port is produced by the ESP32-P4 itself, so it exists only while firmware runs. I have not endorsed the memory-depletion idea, because nothing has measured it.

**Spur suppression** — you were right, and the reason is not what either of us assumed.

The detection is fine. On 20m it found 87 spur bins, strongest 38.5 dB over the noise floor at exactly −8015.6 Hz, second harmonic where it should be.

The menu offered the weaker treatment first. Measured on the waterfall: "Subtract spur power" takes the spur columns down ~28%, "Erase spur bins" ~78%. So "not all that effective" is a fair description of subtract — which is what you were given. In v1.8.4 **Erase comes first**.

Two things about Erase turned out better than expected: no dark notches (it ramps between neighbouring bins rather than blanking them), and the measurement is cached per frequency — the ~3 s nudge happens once, and returning to a frequency is free.

The 3 s on a new frequency stays. A spur's offset moves 16–50× faster than the dial, which is exactly what makes it detectable, and also why the map is invalid after even a 100 Hz retune.

If Erase still underwhelms you, send a before/after screenshot — that would be a different problem from the one I fixed.

**The terminal** you and Michael discussed is in v1.8.4 — see below. Your instinct that it might not fit was reasonable; Michael was right that it is small.

---

## 4. Brian WA6JFK

**The config file that would not take** — there was a real gap. If nothing in an uploaded file was recognised, the page said "Config applied: 0 item(s)", which reads like success. In v1.8.4 it says plainly that nothing was recognised and that nothing was changed, so the next step is to fix the file rather than reset the unit.

The erase install did no harm, but you should not have needed it — v1.8.4 has "Change QRZ API key" and "Change eQSL login" rows.

7 FT8 contacts from a portable vertical after 30 on SSB is a good afternoon.

---

## 5. Randy N4OPI and Michael KZ4LY — the terminal is built

It is in v1.8.4. Tab5: Settings drawer → Radio → "Radio menus". Browser: the Radio menu. Both show the radio's own 80×24 menu screen with arrow keys, Enter and Back.

One thing first: set the QMX's **System config → GPS & Ser. ports → USB serial ports → 2**. One-off, survives a power cycle. Randy, on a headless unit you can do that over CAT.

Why the second port is the whole design: the QMX manual is explicit that leaving a terminal session without choosing "Exit terminal" leaves the radio refusing CAT. On the shared port that would take the panadapter down. On its own it cannot — I measured CAT frequency, mode and the S-meter running normally throughout a session. Closing walks the radio out through its own "Exit terminal", found by reading the screen rather than counting keypresses, so it works however deep you are. Close the browser tab, or leave it two minutes, and it hands the radio back by itself.

Michael — you were right that it is not big. The one thing that turned out load-bearing was reverse video: it is the only marker of which item is selected.

Randy — 12 oz with the QLG3 included is impressive packaging, and it is the case this feature exists for.

---

## 6. Kevin KW6E

Found and fixed in v1.8.4, and your description identified it — that asymmetry can only be one thing.

Your mouse reports movement as 16-bit numbers; the panadapter read them as 8-bit. So the horizontal value it used was the bottom half of the real one, which wraps every 256 counts (the jumping), and the vertical value was the *top* half of the horizontal one, which is nearly always zero. Fast sideways, dead vertically.

It now reads the layout your mouse publishes instead of assuming the older "boot protocol" one.

On the speed/acceleration setting: I deliberately did not add one. No amount of scaling makes one axis wrap while the other stands still, so it would have hidden this rather than fixed it. If the speed still does not suit you once it tracks properly, say so and I will add a real setting.

I do not own a Surface Arc Mouse, so this is fixed against what your symptom implies, not against your hardware. If it is still wrong, send a diagnostic log from just after you plug it in — there is now a line describing exactly how your mouse's layout was read, and that settles it.

---

## 7. Gyula HA3HZ

Thank you for writing these up while they were fresh.

**Close button red like Delete all** — fixed in v1.8.4, and it was worse than you said: Close was the *brighter* red. Red means "cancel" everywhere else, which is fine where nothing is destroyed, but that window is the one place with a delete button. Close is now neutral, so there red means one thing only.

**How much the log holds** — about two thousand contacts. Internal storage is 1 MB shared between the log, the diagnostic log and the LoTW certificate. Download and clear it periodically.

Your question found a real bug: if that storage ever filled, the panadapter failed to write the contact **and still reported it as logged** — you would have found out at upload time. v1.8.4 checks, refuses to count a contact it could not save, and names the station on screen.

**No daily breakdown** — correct, it is one continuous file. The Tab5 viewer has a Today/All filter, but the download is everything. If a per-day export would help for POTA, say so.

**Pileup, two answers, only the first worked** — what you saw is right, and the gap is that nothing tells you. It works them one at a time; if the second is still calling it picks them up, and if it has gone quiet it repeats a few times then gives up — your "reports x times and times out". Neither is wrong, neither is visible. Not fixed: I do not yet have a good answer for what the screen should show. You are the second person to hit it.

---

## 8. Tony Abbey

Roy and Samuel have it right, and I will confirm from my side: this is not the panadapter.

The Tab5's power button is a soft shutdown, not a switch in the battery line, so some drain continues. M5Stack document that it charges only once powered on and initialised — so USB while "off" does not offset that. Below about 6 V the pack goes into protection and needs an external charger; the Tab5 cannot recover it.

So: slide the NP-F550 off for storage longer than a few days, and a flat pack needs a charger made for that type. I will add this to the manual.

If you ever see it discharge noticeably faster on one release than another, tell me — that would be mine.

---

## 9. Michael KZ4LY — crash log (email reply)

Thank you for sending it even though you could not reproduce it.

I could not diagnose it, and that is my fault: the log you sent **cannot** contain the crash. The microSD copy only receives the first few seconds of each boot while WiFi is on — a deliberate choice, because writing to the card with WiFi up is what used to wedge the network on this board. The consequence, which I had not thought through, is seventeen boot headers about five seconds each. It stops before anything interesting, every time. That is worse than no log, because it looks like evidence. Written down as a thing to fix.

What it did show: no crash backtrace anywhere, and watchdog resets rather than software panics. With the light blue screen, that points at something holding interrupts off long enough for the hardware watchdog to fire — the light blue is the display giving up on a frame.

If it recurs, send **qmx-log-saved.txt** from the web page's "Diagnostic download". That one lives in internal flash, keeps appending, and survives a restart.

The 180° rotation is worth mentioning again if it happens once more. No reason yet to think it is involved, but you noticed it was your first time using it.

Your handheld all-in-one: the terminal in v1.8.4 should help — a QMX+ with no panel is now configurable from the Tab5 itself.
