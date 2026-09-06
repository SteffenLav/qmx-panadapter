# Version History

All releases are available on [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases).

## Latest Release

**v1.11.3** — 2026-09-06

**Two things a user told me were still broken after I had said they were fixed. Both of them were right.**

- **The power-cycle relay was held closed from boot** for anyone using an active level of Low *(Randy N4OPI)*. The two pins were driven Low at every startup, under a comment reasoning that closing a contact should require a deliberate pulse — which is right, and is the opposite of what that did for an active-Low station, because Low is their asserted level. On a line wired to a radio's power input the relay therefore sat closed from power-on until the first pulse released it. The pins now rest on the inactive side of the polarity you chose, and follow it the moment you change it.
- **A corrected log file can now be imported** *(Gyula HA3HZ: "the corrected file cannot be installed, the previous incorrect version remains")*. Restoring a log merges on callsign, date and time — exactly the three fields a correction does not change — so a record fixed in another logger looked like a duplicate, was skipped, and was reported as "already in the log". Choose **corrections** at the first prompt and an incoming record replaces the one already logged; the result says how many were replaced, and those contacts go to QRZ, eQSL and LoTW again, since the copy held there is the one carrying the error.
- **The web page tells you when it is older than the Tab5.** A browser tab left open across an update keeps working perfectly while missing every control added since it loaded — which is indistinguishable from a feature that was never shipped, and cost a round trip about an editor that was present all along. It now offers a Reload when the firmware version changes underneath it.


## Previous Releases

**v1.11.2** — 2026-09-06

**A QSO log quick enough to use, a check-my-log pass before you submit an activation, and a choice of frequency punctuation.**

- **The QSO log opens in 0.13 s instead of 2.2** *(Gyula HA3HZ: "it takes so long for it to appear that it is not worth searching for it")*. Measuring first changed the fix — reading the file was only 70 ms of it, building the rows was the rest — so it builds a screenful and adds the rest as you scroll. Nothing is capped; every record is still reachable.
- **The log's search covers the whole log, not just today** *(Gyula HA3HZ)*. Searching a log asks "have I ever worked this", not "did I work it today".
- **"Check log" in the web QSO log** *(Don Adams WB0LQW)*. It lists what each record is missing before you submit an activation — a callsign, a malformed date, no station callsign, a reference that does not look like one, or none of your own. It can only say a record is not obviously incomplete: the Tab5 cannot see POTA's rules or their database, and it never says a file will be accepted. The Tab5's Activation panel shows the same count, so it can be read at the park without a laptop.
- **A station could be logged with another station's grid square**, and that is fixed. A recycled entry in the decode table kept the previous callsign's locator, so a new station could inherit it and keep it if its own messages never carried one — exactly a QRP contact that finishes on reports and RR73.
- **A grid square can be corrected by hand in the web log** *(Gyula HA3HZ, who was editing them in ADIFMaster)*, joining the two reports and the park reference as the fields that may be changed.
- **A choice of frequency punctuation** *(Don N2VGU)* — `14.074.000`, the default and what the QMX shows on its own LCD, or `14,074,000`. It applies to the readout, the preset buttons and lists, the WSPR band picker, the keypad and the spectrum scale.
- **Decoded CW is written to the microSD card with timestamps** *(Michael K Johnson KZ4LY)*, so a half-caught callsign can be resolved afterwards.
- **A download that failed partway was served as a file that looked complete** *(Gyula HA3HZ: "the website shows 220 lines of LOG data")*, with 462 in his log. The same fault was in the diagnostic-log download and the SD file browser.
- **The power-cycle relay's settings survive a reboot** *(Randy N4OPI)*.
- **"Lost contact with the Tab5" no longer appears over a spectrum that is drawing perfectly** *(Dave KX3DX)*, and web spot labels carry the mode as the Tab5's always have.
- **Three crashes fixed**, including one that could abort the firmware when a browser reached the web server in the moment it started — present and unrecognised for many versions. Three tasks that were within a few hundred bytes of overrunning their stacks have been given room, and an overrun is now trapped as it happens.

### v1.11.1 — 2026-09-05

**Decoded CW along the bottom of the panadapter, from the radio's own decoder.**

- **Decoded CW along the bottom of the panadapter** *(suggested by Uwe DL8UG)*. In CW or CW-R, a single line shows the Morse the radio is decoding, with an estimate of the sending speed — on the Tab5 and in the browser alike. **The QMX decodes it itself** and hands the text over the CAT link, so this costs the panadapter no processing at all: it does not touch the spectrum, the waterfall or FT8. Works on QMX firmware 1.03 and later, and on most radios there is nothing to switch on.
- **Noise is filtered before it reaches the screen.** With no signal the decoder produces a stream of rubbish; measured on a live band, `*` — its marker for a symbol it could not resolve — was the single most common thing arriving, and it is dropped outright. Long runs of E and T, the two shortest Morse symbols, are dropped as well, while short ones are kept so that real words survive.
- **The line wraps and overwrites itself** rather than scrolling, so a callsign you are half-way through reading stays where it is. Two spaces travel ahead of the writing position to show where the new text is landing.
- **The speed is a throughput figure**, not the other operator's keying speed: it counts the gaps between words, so it reads low during a real exchange. Shown with a `~` for that reason, and `??` when there is not enough to go on.
- **Turn it off** in the settings drawer under Radio, beside CW centre and the transmit offset. On by default, and the setting is shared with the browser.
- **Restore from SD now reads both logs on the card** *(Gyula HA3HZ)*. It only ever read `qso.adi`, so putting a backup on the card as `qso.prev.adi` — the name the firmware itself writes — was met with "nothing to restore". It reads both now.
- **The Tab5's QSO log gains a Grid column**, beside the callsign.
- **The power-cycle relay's help text no longer names a connector that does not exist** *(Randy N4OPI)*. The QMX has no PWR_ON/GND jack — those signals have to be brought out to a connector of your own — so the control now says so, and says it is an experimenter feature.

### v1.11.0 — 2026-09-05

**Your QSO log, recoverable from the card it was already backed up to — and searchable on both screens.**

- **Restore your QSO log straight from the microSD card** *(Gyula HA3HZ)*. The card has always held a copy of the log, and until now the Tab5 could only ever write to it. Gyula lost his log to a firmware reinstall with 432 contacts sitting on the card, inside the radio's own screen, and no way to reach them without a computer — he reasonably assumed the device would notice them. It does now: **Restore from SD** in the Tab5's log window, or **↳ Restore from SD card** in the browser's QSO Logs menu. It merges, so contacts already logged are skipped and pressing it twice does nothing.
- **The card keeps the previous log as `qso.prev.adi`.** The card mirrors the *present*, so a QSO deleted before a restart used to be gone from the card as well at the next start-up. Whenever the log about to be written is smaller than the one already there, the older copy is kept first — normal logging never disturbs it, so it holds the last larger version for as long as it takes you to notice.
- **Search the QSO log, on both screens** *(Gyula HA3HZ)*. Type any part of a callsign, country, band, mode, date or park reference and the list filters as you type; several words must all match. If nothing matches it says so — *"so this one has not been worked"* — which is the question worth asking of a log. The decode list already greys out a station you have worked, but only while that station is on the air; this asks the same question whenever you like.
- **Export just the contacts you pick** *(Gyula HA3HZ)*. In the browser's log viewer, tick rows and press **Export selected** to save them as their own ADIF file. The tick box in the header takes everything currently shown, so a search plus one tick gives you a single day, band or park. Each record is exported exactly as the Tab5 wrote it, so nothing the table does not display is lost.
- **A finished QSO now waits for you in the browser** *(Randy N4OPI)*. `<callsign> QSO complete` stays on screen in green until you do something else, instead of clearing itself after twenty seconds like every other message there — so stepping away and coming back still tells you the contact finished. The radio does not wait; only the message does.
- **A restore or a delete on the Tab5 reports in a window with an OK button**, not a message that fades on its own. The result of something you asked for is worth reading. Clearing the whole log now says "This cannot be undone" in as many words.
- **The Tab5's log gains a Ref column** — the park or summit the other station was activating — because the new search offered to find it while the list never showed it.
- **An import that could not read a file no longer claims everything was already logged.** It now reports how many contacts were found, added, already present and unreadable. A file too large gives a plain sentence instead of a raw browser error, the size limit is four times higher, and a slow upload is retried rather than abandoned.

### v1.10.9 — 2026-09-05

**The web decode-list jump, root-caused for real this time; WSPR's PA-voltage guard made reliable; a remote QMX power-cycle relay.**

- **The decode list no longer jumps, for real this time** *(Randy N4OPI)*. v1.10.8's fix sized the status box against one "worst case" message and still moved on an armed transmit or a busy exchange — the real cause was the box disappearing from the page entirely while idle and reappearing at full size once there was something to show. Every hide path now leaves its space reserved instead. The countdown shown while armed is its own small figure that can't be cut off, Cancel clears immediately with no leftover text, and the box no longer runs wider than the slot occupancy strip below it.
- **The "Calling you" pileup list ages out and clears on a band change** *(Randy N4OPI)*. It previously had no expiry at all.
- **WSPR's finals-protection PA-voltage guard is confirmed and retried, not fire-and-forget** *(Dirk DK7CVD)*. Restoring the radio's power on leaving WSPR was a single CAT write with nothing checking it landed; a background check now confirms and resends if needed. Verified on real hardware with an 11.5 V → 6.0 V → 11.5 V round trip against the radio's own read-back.
- **The WSPR countdown no longer appears to hit zero and restart on the first cycle.** The PA-voltage question is now asked the moment WSPR transmit is turned on, giving it the full two minutes to be answered instead of a few hundred milliseconds.
- **New: a remote relay pulse for power-cycling the QMX** *(Randy N4OPI)*. "Power-cycle relay" under the web UI's Miscellaneous menu — wire a home-automation relay to it and the QMX's PWR_ON/GND **signals**, and a remote firmware upgrade no longer needs someone at the bench. **The QMX has no PWR_ON/GND jack** — those signals have to be brought out of the radio to a connector of your own first, so this is an experimenter feature *(Randy N4OPI)*.

### v1.10.8 — 2026-09-03

**A crash introduced and fixed in the same release cycle, plus more groups.io reports.**

- **A settings-drawer scroll could crash the device.** v1.10.7's "while the drawer is scrolling, nothing else in it acts" fix forced the touch driver to treat a finger as already lifted the moment a scroll began, and that collided with LVGL's own built-in momentum-scroll animation on the same object — every reproduction crashed the display task at the identical point. Found with a diagnostic build that confirmed the cause before anything was changed, not guessed at. The offending call is gone; the drawer's scroll-vs-tap protection is unchanged and still works.
- **Restore your worked-station history from a downloaded ADIF file** *(Randy N4OPI)*. A clean erase-and-reinstall used to leave no way back to it — restoring settings has never touched the QSO log, on purpose — so **ADIF restore** in the web UI's QSO Logs menu now merges a previously downloaded (or any logger's) file in, skipping anything already logged.
- **The web decode list no longer jumps up and down during an exchange** *(Randy N4OPI)*. The status box above it used to grow and shrink with the message text, pushing the list around while you were trying to click a row.
- **The web spectrum could go on drawing against a stale frequency axis** *(Samuel W7STF)*. If the once-a-second status poll missed a beat after a band change or mode switch, the picture kept looking normal while every signal sat at the wrong frequency — it now blanks itself and says why instead. Not yet confirmed on the air.
- **A WSPR spot hopped to a new band could be published to wsprnet.org under the wrong band** *(Kevin KQ4DTX)* — the upload read the dial at send time rather than at decode time. Not yet confirmed on the air.
- **The "QMX cannot display this" caption is gone** from the hatched dead-band on both screens — the hatching alone says what it needs to.

### v1.10.7 — 2026-09-02

**User reports, and one crash that had been in the firmware for months.**

- **A crash that rebooted the Tab5 during FT8 is fixed** — and it was our own leftover debugging code. A routine from earlier weak-signal development was still trying to open a log file **once per decoded signal, up to 140 times a slot, every slot**, on a path that could never be created. Each attempt asked for a small piece of memory the device was short of, and eventually one failed and the firmware stopped. It looked like a cyan screen and a restart, usually a minute or two into an FT8 session.
- **Static IP addressing works.** It did not in v1.10.6 — the address was always rejected and the Tab5 fell back to DHCP without saying so.
- **The browser's band dropdown no longer switches you to FT4 by mistake** *(Steve KX7R)*. Its FT4 list had been filled with the FT8 frequencies, so any preset you picked switched the radio to FT4, with no way back from the browser. There are separate FT8 and FT4 groups now, and the mode shown comes from the radio.
- **A timed-out contact no longer blocks the station.** It stayed on screen until tapped, and while it was there the automatic answering would not start anything new. It clears itself after 20 seconds, counting down as it goes.
- **WSPR transmit is off every time you open the page**, rather than resuming because of how you left it last time.
- **The WSPR countdown counts down to a real transmission** *(Roy KI0ER)*. It used to count to the next opportunity and restart whenever the duty cycle decided not to transmit.
- **The first WSPR burst of a session can no longer go out at full power** *(Roy KI0ER)*, and **an interrupted session no longer leaves the radio at a quarter power in every mode** — the PA voltage is restored when the radio reconnects, and only if that radio is still sitting at the reduced value, so one radio's setting can never be pushed onto another *(Michael KZ4LY)*.
- **The web page gains a large MODE heading**, the **slot occupancy strip and slot countdown** on FT8, and the **whole WSPR left-hand panel** — band, transmit state, finals-guard voltage, best DX, stations per cycle and wsprnet status. Both panels also stop shifting about as decodes arrive.

### v1.10.6 — 2026-09-01

**A release of things users found — almost every item here came from a report on the air.**

- **WSPR: the waterfall no longer goes blank for two cycles around a transmission** *(Dirk DK7CVD)*. Only the transmitting cycle is dark now. The transmitter was being armed up to two minutes before its slot and the receiver stood down for that whole wait as well, so at a 50% duty cycle the band went unheard about half the time it looked like it was listening.
- **WSPR: the TX button stops a transmission at once** *(Roy KI0ER)*, instead of doing nothing visible until the end of the cycle.
- **WSPR: leaving the mode gives the radio its power back** *(Roy KI0ER)*. With "Protect finals" on, the reduced PA voltage was being left behind, so CW and FT8 ran at a quarter power with nothing on the Tab5 to say so — visible only on the radio's own Protection menu.
- **WSPR: the spot list fits the screen.** The right-hand column was running off the edge. Headings now sit over their own columns, **M** is headed **BAND**, and long country names are shortened rather than replaced by a two-letter code.
- **Picking a spot brings it into view** *(Roy KI0ER)*. With the still display on, a chosen spot could land right at the screen edge — reliably so, given the view only re-frames when something reaches an edge.
- **The still display holds completely still, then jumps** *(Dirk DK7CVD)*. It used to hold, then be dragged along by the tuning, then jump, so the empty edge kept changing size. At most half your filter width now slides off before it re-frames.
- **A static IP that would lock you out is refused**, and blank mask/gateway/DNS are filled from the current DHCP lease rather than assumed. There is also a **"Use DHCP" button on the Tab5** *(Michael KZ4LY)*, shown only when a static address is set — until now the only way back was the web page the wrong address had just made unreachable.
- **A QMX restart could reboot the Tab5.** Worth knowing if you have seen this: the reboot also disturbs the radio, so what looks like the QMX wedging can be the Tab5 restarting underneath it.
- **The microSD card is handled better.** Its first write no longer collides with WiFi starting up, a failed background write no longer unmounts a working card, and the card is retried for an hour after start-up — so **a card inserted while the Tab5 is running is now picked up** within about five minutes instead of being ignored until the next restart.

### v1.10.5 — 2026-09-01

**The spectrum holds still while you tune across it, and a quarter of the ×1 view stops lying about where signals are.**

- **A still spectrum and waterfall.** The panadapter now behaves the way a Flex does: the spectrum and waterfall stay where they are and the VFO marker moves over them, so a signal stays put on screen while you tune towards it. It also makes the waterfall readable as history — a signal's past sits directly above its present, under the frequency it belongs to, instead of the whole picture sliding sideways every time you touch the dial. The view re-frames only when you tune far enough to need it: a dead band where nothing moves, a small push so a station sitting at the screen edge can still be worked, then a page carrying part of the old screen across so you can see where you came from. What triggers it is **your filter passband reaching the edge of the screen** rather than a fixed percentage of the view — the passband is not centred on the dial, so a percentage rule re-frames too early at one edge and too late at the other, and mirrors itself in LSB. On by default; switch it off under **Settings → Radio & display → Still spectrum**.
- ⚠ **At ×1 the display stays centred on the dial**, whatever that setting says. Holding a view still needs somewhere for it to stay while the capture window slides underneath, and at ×1 the view is already the whole 48 kHz the radio sends. Zoom to ×2 or beyond for the still display.
- **The right-hand quarter of the ×1 view was showing real signals at the wrong frequency.** The QMX's local oscillator sits 12 kHz below the dial, so the 48 kHz it delivers covers dial−36 kHz to dial+12 kHz and there is nothing at all above dial+12. The display filled that quarter by wrapping the bottom of the band into it, and the frequency scale labelled it as dial+12 to +24 — so the signals shown there were real, but about 48 kHz from where the scale claimed, and **tapping one tuned you to the wrong place entirely**. That region is now hatched and inert on the Tab5 and in the browser, with a caption saying why it is empty.
- **Leaving FT8 or FT4 could reboot the device a few seconds later.** Switching back to the panadapter tore down the decoder while one of its two decode tasks was still working on the last slot, and the memory it was reading was freed underneath it. The window is easy to hit precisely because you are going back to the panadapter: that puts the display work back on the same processor core the decoder shares, so the decode finishes more slowly at exactly the moment the teardown is waiting for it. The decoder now abandons the final slot's work as soon as you leave — those decodes were about to be discarded anyway — and the teardown refuses to free anything it cannot prove is finished with. **This bug is older than v1.10.5**; it was found while testing this release.
- **Changing band ends a contact in progress** *(Randy N4OPI)*. Switching bands used to stop the automatic picker choosing a new station but leave any exchange already running untouched — so it kept sending its next message on the new band, to a station no longer there. Changing band now ends the contact as well.
- **The WSPR transmit button has moved clear of the left edge** *(Randy N4OPI)*. It overlapped the edge-swipe strip used to reach the panadapter, so a swipe that started a little high could land on a control that keys the radio for about 110 seconds.
- **WSPR spots show the band they were heard on** *(Roy KI0ER)*, in a new **M** column. With band hopping on, a list of spots from several bands could not be read otherwise.
- **The WSPR waterfall marks a transmit cycle** *(Dirk)*. On a cycle you transmit, the receiver is stood down for the whole two minutes and there is nothing to draw — so the previous cycle's picture used to sit there looking frozen. The display now lays down its cycle-boundary marker and keeps the last received image below it.
- **The browser gets the FT8/FT4 band preset list** *(Randy N4OPI)* that the Tab5 has always had.
- **A LoTW upload now shows LoTW's own reply** *(Randy N4OPI)*. The count reported before was our count of what was *sent* — LoTW accepts a file and processes the contacts afterwards — so an upload could report success while nothing appeared in the log. The server's own message is now shown, which is what says whether anything was actually rejected.
- **The LoTW certificate can be replaced from a visible button.** Re-importing it used to need a Ctrl-click nobody would guess at, and a certificate expires about every three years.
- **A static IP address** can be set under **Settings → WiFi** — address, mask, gateway and DNS. Leave the address empty for DHCP, which is what every unit does today, so nothing changes unless you fill it in. ⚠ Get the subnet right: an address that is valid but on the wrong subnet leaves the Tab5 unreachable, and the web page is the only place to change the setting back.

### v1.10.4 — 2026-08-30

**FT4 replies are quick again, and the Tab5 stops inventing an FT4 power reading.**

- **FT4 answers in the right slot** *(Gyula HA3HZ)*. FT4 now waits for the current slot to finish decoding before it transmits, the same rule FT8 has used since v0.21.0, so a reply goes out in the slot it belongs to instead of a cycle later. It should feel at least as quick as FT8. This was tried in v1.10.2, broke FT4 transmit entirely, and was withdrawn in v1.10.3 — it is back, this time with the transmit timing checked by an automatic test rather than by eye.
- **FT4 no longer reports a made-up 5.0 W** *(Gyula HA3HZ)*. An FT4 symbol is 48 ms and asking the radio for its power can take 50 ms, so the power cannot be sampled during an FT4 transmission the way it is in FT8 — and the display was filling that gap with a fixed 5.0 W. The radio's own reading was the correct one. **FT4 now shows no power rather than a wrong one**; FT8 is unaffected and still measures properly.
- **Auto-work pileup leaves busy stations alone** *(Gyula HA3HZ)*. A station who had called you could be answered a couple of minutes later, by which time they were already in a contact with somebody else — and then called repeatedly. If their last message shows them working another station, they are now skipped until they are free.

### v1.10.3 — 2026-08-30

**FT4 transmits again — update if you use FT4.**

- **FT4 would not transmit at all** *(Gyula HA3HZ)*. In v1.10.2 an FT4 transmission was held back at the start of its slot and then never sent: the countdown ran normally, the message was armed, and nothing went on the air — on a CQ and on a call alike. FT8 was unaffected throughout. Fixed, and FT4 now transmits at the start of its slot as it did in v1.10.1.
- **The FT4 reply timing from v1.10.2 is withdrawn with it.** That release made FT4 wait for the current slot to finish decoding before transmitting, so a reply landed in the right slot instead of a cycle later — and that is exactly the change that broke transmitting. FT4 replies can again be a cycle late. It needs a transmit window sized for FT4's shorter slot, which is being done properly rather than quickly.

### v1.10.2 — 2026-08-30

**Bluetooth keyboards work, and two logging faults are fixed.**

- **A Bluetooth keyboard now types into every field** *(Don N2VGU)*. Pair one and it works everywhere the snap-on keyboard already did: every text field, Enter and Esc in every window, Tab to move between fields, and the arrow keys. The on-screen keyboard steps aside while a Bluetooth keyboard is connected, which is the point of having one - it gives you back the screen space it was covering.
- **A keyboard and a mouse can be connected at the same time.** Either one first, the other after, and both keep working. If your keyboard sleeps, it reconnects on the first keypress and anything you typed while it was waking is delivered rather than lost.
- ⚠ **US keyboard layout only for now.** A Bluetooth keyboard reports key *positions* rather than characters, so letters, digits, Enter and the arrows are correct on any keyboard, but punctuation on a non-US layout will not be. National layouts are coming.
- **A logged contact could be missing the received signal report** *(Gyula HA3HZ)*. If the other station's message already carried their report of you, the Tab5 replied and moved straight to the roger step - and their report, which was sitting in that very message, was never written to the log. It is recorded now. Contacts already in your log are unaffected; this applies to new ones.
- **FT4 replies no longer go out a cycle late** *(Gyula HA3HZ)*. FT8 waits for the current slot to finish decoding before it transmits, so a fresh reply lands in the right slot. That had never been switched on for FT4, which is why FT4 exchanges took roughly twice as long as they should. Now on for both.
- **The RIT indicator says what it is doing** *(Don N2VGU)*. With no offset engaged it read simply "RIT", which is easy to mistake for switched on. It now reads **RIT OFF** in grey with a line through it, so all four states - off, armed, engaged, and parked - state what they are rather than naming the feature.
- **Declared WSPR power set from the browser now shows on the Tab5.** The Tab5's own dropdown kept the value it was built with, so the two screens could disagree about a figure that is published with every spot.

### v1.10.1 — 2026-08-29

**WSPR now protects your radio's finals, and the declared power stops being a guess.**

- **Protect finals, on by default.** A WSPR transmission keys the radio for about **110 seconds out of every 120** - an FT8 burst is about 12 - and running a QMX flat out on that cycle puts sustained heat through the PA transistors. QRP Labs warn about exactly this in the QMX manual, and the radio's own built-in WSPR beacon turns its PA down for the same reason. The Tab5 now does the same: it sets the QMX's *Max. PA voltage* to about 6 V for as long as WSPR transmit is enabled, and restores your setting afterwards. **Measured on a QMX at 12 V: 5.4 W → 1.6 W out, and 76% less heat in the finals.**
- **You can always see which state you are in.** The control is a full-width button reading green *"ON - about 1 W"* or red *"OFF - FULL POWER, finals at risk"*. Turning protection off takes two deliberate taps; turning it back on takes one. While it is off the TX block on the WSPR page reads **FULL PWR** in red.
- ⚠ **It reduces the heat in the finals; it does not remove it from the radio.** The excess is dropped inside the QMX instead, so total heat fell only 18% in the same measurements. **If you intend to beacon for hours, feed the radio from a lower supply** - the QMX accepts 6.0-12.0 V, and around 9 V leaves far less to throw away as heat. That is the one thing no firmware setting can do for you, and it is now in the manual.
- **Declared power is advised by measurement.** During each transmission the Tab5 asks the radio what it is actually producing and shows the answer under the setting. Switching protection on or off also moves the declared figure to the value that setting normally gives. Both are suggestions - the number is a claim about your station and stays yours to choose. The list runs to 37 dBm again: a declared power never commanded the radio, so limiting it could only have prevented an honest declaration.
- **The FT8 Options checkboxes are easy to hit** *(Don WB0LQW)*. The touch target was the small box alone, a tap that drifted a few pixels was swallowed by the panel behind it, and the word beside each box did nothing. The touch area is much larger now, the tap cannot be stolen, and **tapping the word toggles the setting**.
- **The WSPR settings are reachable from the browser** for the first time, and the web and Tab5 lists agree.

### v1.10.0 — 2026-08-28

**WSPR, and a settings drawer you decide the shape of.**

- **WSPR — a third page.** Swipe now cycles **Panadapter → FT8/FT4 → WSPR**. WSPR is a propagation beacon rather than a contact mode: you transmit a very slow, very weak signal carrying only your callsign, grid and power, and stations worldwide report hearing it. Over an evening you get a picture of where your antenna and your band actually reach, at power levels where nothing else would be heard at all. Nobody replies, and nothing goes in your log.
- **The page** shows the stations heard in each two-minute cycle with distance and bearing, the furthest of the session, a per-cycle history so an opening band looks different from a closing one, and the captured 200 Hz window. Receiving is the default and is worth doing on its own; transmitting is opt-in and, like FT8, refuses to key without your callsign and grid.
- **Its settings live in the drawer**, under **WSPR**, and appear only on that page: allow transmitting, declared power, duty cycle, band hopping, and whether to publish what you hear to wsprnet.org. The page itself keeps only the TX switch — duty cycle and band hopping are decisions made once for a session, not controls you reach while watching spots arrive. Ticking two or more bands in the picker is what turns hopping on.
- **Declared power is a claim, not a measurement.** The Tab5 cannot know what your radio delivers, and every spot publishes that number worldwide into a database other operators reason from. Set it to what your transmitter really produces.
- **The Tab5 now wakes up on the page you left it on**, including after a firmware update. If that page is WSPR with transmitting enabled, the station resumes beaconing on power-up — which is what a beacon is for, but worth knowing before you leave the shack.
- **Basic and Advanced.** The drawer's EXPERT button is now **ADVANCED**, which names the contents rather than the reader. More usefully, which settings appear in which view is no longer fixed: the web UI's Settings window has a **Tab5 config** button opening a table of every section with Basic and Advanced ticks. Basic holds the things an operating session actually reaches; everything remains in Advanced. A firmware update that adds a setting will show it rather than hiding it behind a layout saved before it existed.
- **Two field reports from Gyula HA3HZ**, both fixed. A station you had just worked could be called again within minutes — the decode list greyed the callsign while the engine ignored it unless a filter was ticked, so the screen and the machine disagreed. The automatic pickers now leave a station alone for 30 minutes after working it, whatever the filter says. And the red **FREQ BUSY** warning had no signal-strength test at all, so a barely-audible station on the other side of the world raised the same alarm as a loud neighbour; it is now graded by strength, and hidden entirely during an exchange, where your transmit tone is deliberately locked to your partner.
- **Also:** the Tab5's frame rate and redraw load are now in the diagnostic log, Bluetooth's host task moved off the display core, several task stacks were returned to the pool, and a WSPR session no longer leaves the FT8 page tuned to the WSPR frequency.

### v1.9.6 — 2026-08-26

**POTA and SOTA logging put right, the mouse wheel tunes, and the phone menus work again.**

- **Your callsign is now written as `STATION_CALLSIGN`**, the field POTA reads *(Don Adams WB0LQW)*. It used to warn *"No station_callsign field, assuming operator …"* on every upload and guess.
- **Park-to-Park and Summit-to-Summit contacts can be entered afterwards** — a new **P2P ref** column in the web log editor, where you type just the reference and the Tab5 works out the programme.
- **FT4 is logged the way the ADIF specification defines it** (`MODE=MFSK`, `SUBMODE=FT4`), so editors like ADIFMaster will open the file. LoTW uploads are unchanged.
- **The mouse wheel tunes the radio** over the spectrum and waterfall — 10 Hz a click in CW and the digital modes, 100 Hz in SSB *(Roy KI0ER, John Dusek)* — and it no longer scrolls panels into blank space.
- **The bottom-bar menus work on an iPhone again** — a line added for portrait mode in v1.9.4 hid them behind the page in Safari *(Travis AK6TB, Randy N4OPI)*.
- **"Check for updates" stops saying you are up to date while it is still asking** *(Michael KZ4LY, Samuel W7STF)*, and **background downloading is now a switch** you can turn off while still being told a new version exists.
- **Coming back from the radio's own menus restores your frequency and mode** — they could leave the radio on 160 m *(Randy N4OPI)* — and **Basic/Expert is remembered** *(Samuel W7STF)*.

### v1.9.5 — 2026-08-25

**A fast-follow patch: two ADIF logging bugs, no new features.**

- **A duplicate contact could falsely claim your park was activated** *(Eric, GitHub issue)*. Working the same station twice made the device say "10 contacts, park activated" while POTA.app credited only 9 unique stations and rejected the upload - cost three activations in one outing before it was noticed. A station now counts once toward the 10-QSO (SOTA 4-QSO) minimum no matter how many times you work them.
- **Deleting a single QSO record could silently do nothing.** The delete never checked whether its rewrite actually succeeded before committing it, so a storage write failure looked like the delete "worked" while the record stayed put. It now verifies the rewrite, refuses to touch the log if it can't, and automatically repairs/reclaims space from storage that's become fragmented over a long uptime - so a delete that would previously have failed now just works.

### v1.9.4 — 2026-08-25

**The FT8-specific settings get their own home, and the web page finally works on a phone held upright.**

- **FT8 Options.** The Tab5's **Filter** button is renamed **Options** *(Roy KI0ER)* - it already held real behaviour toggles, not just filters, so the old name undersold it. The web UI gets the equivalent for the first time: CQ message presets and the FT8 filters, previously buried in one long general Settings list, now live behind their own **Options** button next to **TX tone**, shown only in FT8 mode. The button carries a **count** of currently-active settings instead of a plain colour *(Dirk DK7CVD, Roy KI0ER - a colour would always read "active" for anyone who runs filters permanently and tell them nothing new)*, and hovering it lists which ones. Inside the panel, an active checkbox gets a coloured border in addition to its own tick mark, so nothing depends on colour alone *(Don N2VGU)*.
- **The TX tone picker stops going stale.** It used to read the band's occupancy once, on open - a slow decision could cross a 15-second slot boundary and land you on a slot that filled in while you were choosing. It now re-checks every 3 seconds while the picker is open, without disturbing your own in-progress pick.
- **The web page works on a phone held upright.** In portrait, the top and bottom bars could lose controls off the edge of the screen with no way to reach them - landscape was always fine *(Randy N4OPI, iPhone Safari)*. Both bars now scroll sideways, the same swipe as the decode list.
- **Battery reading simplified.** The raw cell voltage is gone from both screens - the percentage already carries the level. In its place: once your charge limit trips, the reading says `(limit)`, on the Tab5 and the web page *(Don N2VGU)*.
- **The snap-on keyboard can be attached - or reattached - any time**, not just at boot. It's found within a couple of seconds whenever it's plugged in, and a detach-then-reattach mid-session works the same way. Its two LEDs stay dark once it's found.
- **The macOS/Linux flasher script is guaranteed clean line endings** regardless of what machine builds the release *(Michael K Johnson KZ4LY, Fedora)*.

### v1.9.3 — 2026-08-23

**Updating is now one decision instead of a procedure.**

- **The update downloads quietly in the background** and only asks you once, at the end. On by default; switch it off under **Settings → Network → Download updates automatically** if you are on a metered connection, since each update is about 3.3 MB. Downloading never installs anything - only a restart does, and only you can ask for that.
- **A proper window in the middle of the screen**, with the version, what will happen, and two buttons: **Restart now** or **Later**. The bottom bar breathes gently while an update waits for you and goes quiet once you have said "later" - a signal that never stops being a signal.
- **The long-press is gone.** A plain tap opens the window. The hold only ever existed so a stray brush could not start a download; now that a press just opens something you can dismiss, it does not need to be defended against. *(Don N2VGU spotted that the old wording described the wrong action at the wrong moment - he was right about the cause, not just the words.)*
- **The band-plan strip is far easier to hit.** It is only 22 px tall with the bottom bar hard against it below and the waterfall above; its touch area now reaches 50 px up into the waterfall while it still draws the same size.
- **The spectrum, waterfall and FT8 decoding keep running while an update downloads.** Previously everything stopped for the whole download. Expect a slight stutter and one brief pause right at the end.
- **Audio is no longer dropped while a log upload is running.** Uploading to QRZ, eQSL or LoTW quietly interrupted the audio feed, which could cost you FT8 decodes at the time.
- **TXCQ ANY / EVEN / ODD on the web FT8 page** *(Randy N4OPI)* - choose which 15-second slot your CQ goes out in, from the browser. Same setting as the Tab5's own button, so the two always agree.
- **SSB tune snap is now 500 Hz** *(Dave KX3DX)* - stations that stray off an integer kHz sit at 0.5, and a 1 kHz grid cannot reach them. Also half as many stops across a drag.

### v1.9.2 — 2026-08-22

**A field-report release: five things fixed or added, three of them from Randy N4OPI.**

- **A stuck exchange that never logged** is fixed *(Roy KI0ER, working K7FD)*. When a caller's own first message back to us was already a signal report (not a fresh CQ), the QSO machine correctly built the reply but started itself in the wrong state - "waiting for their first report" instead of "waiting for RR73" - so it had no idea what to do with a bare RRR and kept re-sending the same reply for over ten minutes. Fixed at the single choke point all three ways of starting this kind of exchange share.
- **The SWR-protection fault is visible from the web page now** *(Randy N4OPI)*. Before this, tripping SWR protection stopped transmission with no explanation anywhere but the Tab5's own prompt, and no way to clear it remotely - the web page showed only a bare "QSO Cancelled". It now shows the same fault message and clears the same way a tap on the Tab5 does.
- **"Who is hearing me" gets time windows and sortable columns.** 15 min / 30 min / 6 h / 24 h chips, and every column header sorts by clicking it - both pure re-slices of the one fetch the device already makes. The FT8/FT4 decode list in the browser gets the same sortable columns, with a "CQ callers on top" link back to the device's own priority ordering.
- **Working an older pileup caller from the web page now actually works** *(Randy N4OPI)*. Clicking a "Calling you:" entry used to refuse outright the moment the caller's row aged out of the live decode table, even though the Tab5's own pileup screen could work the identical caller fine. It now falls back to the same report-first reply the Tab5 has always used. Pileup entries show their age on both screens now, and the Tab5's decode-list HRD column is now **AGE in seconds** - a more useful number for judging how much to trust a row. How long a row survives before it drops off the list is now operator-tunable too: a "Max age:" dropdown in the Options modal, 30 to 90 seconds.
- **Simulation mode no longer leaves real stations flickering on screen.** Turning it on now clears the decode list and pileup immediately, same as turning it off already did, and real decodes are suppressed from the shared list for as long as sim mode is on - a QMX still attached and receiving can no longer keep re-populating a practice session with genuine stations.

### v1.9.1 — 2026-08-21

**A hotfix: updating from the device did not work over a real download, on any version, ever, until this fix.**

- v1.9.0 was meant to be the first release where the OTA offer could actually be used; testing it for real for the first time turned up two bugs.
- **The device could not even open the connection.** GitHub's own redirect response carries more header data than the firmware's HTTP client was sized to receive - every download failed instantly. This is why v1.9.0's offer downloaded nothing for anyone who tried it.
- **Once that was fixed, a second bug appeared: the device could crash and reboot right at the end of a successful download.** The same fix made each downloaded chunk larger, and the download loop never yielded control back to the rest of the firmware while processing it - audio and FFT were starved badly enough, for long enough, to eventually trip a hardware reset.
- Both fixed and **verified on hardware**: a real download against the live release completed twice, with a full serial capture proving zero crashes and zero audio interruption throughout.
- **If you are on v1.8.9 or v1.9.0, this needs one cable flash.** Your device's own broken download code cannot fetch any release, including this one - it fails the same way regardless of what is on the other end. After that one flash, OTA works normally from then on.
- Everything else is unchanged from v1.9.0 - the faster web page, WiFi power saving, the WebSocket eviction fix, and the snap-on keyboard shortcuts. This release exists solely to fix the OTA download path.

### v1.9.0 — 2026-08-21
**The web page is fast again, the snap-on keyboard drives the whole app, and updating from the device can finally be seen working.**

- **Try the update.** v1.8.9 added updating from the device, but the offer only appears when a *newer* release exists — and at the time there was none. Now there is. Hold the version at the bottom of the Tab5, or tap it in the web page. **Do not want to wait for the 30-minute check?** Hold the version even while it says you are up to date and it checks there and then, showing `checking...` while it does.
- **The web page loads in under two seconds instead of about ninety.** It was being sent uncompressed — 263 KB, on a link shared with the live spectrum stream — and is now 83 KB with a tag so a reload costs almost nothing.
- **WiFi power saving is off.** It is right for a battery sensor and wrong for something serving web pages: the radio slept between beacons and relied on the access point to hold anything arriving. Outbound traffic was never affected, which is why the spot feeds always worked while the web page did not. Measured over 500 samples, requests that failed outright went from **14.4% to 0.4%**.
- **The web page was killing the very stream it displayed.** The web server evicts its least-recently-used connection when it runs short of slots, and a connection's place in that queue is only refreshed when it *receives* something — the spectrum stream only ever sends. The browser's own polling was enough to get it thrown out every few seconds. That is the "reconnecting" some of you have seen for a long time.
- **The snap-on keyboard drives the app.** It works in the radio's own menus (arrows, Enter, digits, backspace, Esc to leave), Enter and Escape work in every window with buttons, arrows and Page Up/Down scroll the drawer and the manual, and **Ctrl+R / M / L / K / P / F / S / H / D** open the common screens. **You can reassign all of it** from the web page — 25 actions, with Alt left free for your own. *(Don N2VGU)*
- **Five fixes from Randy N4OPI.** The **TX power and SWR** readout was hidden behind the exchange status, and that whole strip is rebuilt to sit across the panel instead of stacked down it. The **TX tone picker** picked a slot one or two to the left of the click. **"Busy: working …"** and **"QSO cancelled"** never cleared. A **worked station stayed green** in the decode list. And **"Who is hearing me" was quietly discarding reports** — it stopped after 64 and said nothing; it now handles 128 and says when there were more.
- **The decode list shows the country, spelled out**, where it used to repeat the grid square already in the message. *(Randy N4OPI)*
- **The web page no longer asks you to confirm ordinary operating actions** — calling CQ, cancelling mid-QSO, working a station, starting Antenna Tune. Deleting things and installing firmware still ask.


### v1.8.9 — 2026-08-20
**The Tab5 updates itself now, and a transmitter that could be left keyed no longer can.**

- **The radio can no longer be left transmitting.** *(Roy KI0ER)* His QMX transmitted continuously until he power-cycled it, with the Tab5 running normally throughout. A USB timeout mid-burst made every command fail — including the two that *stop* transmission — the burst finished, and although the link recovered two seconds later nothing ever re-sent them. The stop command is now retried immediately, and if it still cannot get through it is handed to the radio-control task, which keeps re-sending it on every cycle that works until the radio is demonstrably back in receive.
- **Updating from the device.** The version at the bottom of the screen tells you when a newer release exists and installs it if you ask: touch it, hold for a second, let go. It downloads in the background and never restarts on its own — you choose when. The web page says the same in the same words. Nothing is downloaded unless you ask, your settings and logs are untouched, and the previous firmware is kept as a fallback. Install this release with the flasher as usual; after that the cable is only for emergencies.
- **A crash now survives the reboot.** An unexpected restart can finally be diagnosed from a diagnostic download — the next boot records what happened, which part of the firmware, and how far into the session.
- **The microSD card keeps the diagnostic log again while WiFi is on.** Previously it only received a backup at start-up, so the log was not there when it was needed.
- **The flasher download is 2.9 MB instead of 44 MB.** *(Gyula HA3HZ)* It had been quietly carrying every previous version inside it.
- **FT8 can move off an occupied frequency mid-QSO** — to the nearest clear slot only. *(Roy KI0ER, Gyula HA3HZ)* Far enough to escape whoever is on top of you, near enough that a station using a narrow receive filter still hears you.
- **Bandwidth stayed on a CW filter after switching to LSB.** *(Samuel W7STF)*
- **The out-of-band tuner now works in the browser too.** *(Samuel W7STF)*
- **Browser display stalls.** *(Samuel W7STF)* The message telling a second browser it had lost the live view was malformed, so browsers hung up and grabbed it straight back.
- **The frequency readout could stick** on an old value while the spectrum and the radio were both correct.
- **Spur suppression has been withdrawn** from the settings drawer. It only ever worked at ×1 zoom, which is not where anyone looks at spurs, and with a real antenna the problem is far smaller than bench measurements suggested. The work is kept for a future release.
- **Every web API command is documented**, and the error behaviour is now described correctly.

### v1.8.8 — 2026-08-20
**If the Tab5 ever restarts on you, the diagnostic download now says why.**

- **A crash now survives the reboot.** Until now a crash left nothing on the device — the details went straight out of the serial port and were gone, so if you sent a diagnostic download it contained everything except the one thing needed. The next boot now reports the previous crash: what happened, which part of the firmware, how far into the session, and where. **If your Tab5 restarts unexpectedly, just send the diagnostic download.** A restart with no crash record is also positive evidence it was *not* a crash — a power cut looks identical otherwise.
- **An FT8 reboot open since v1.3.0, root-caused.** Entering FT8 could start the decoder twice, and the second copy freed memory the first was still using. The cause was an internal "is it running yet" flag that answered a moment too late.
- **FT8 was quietly discarding decodes** — 99 out of 54,142 in a measured run. A dropped decode never reaches the list, the busy-frequency map, or the check for a reply addressed to you, and a lost reply looks exactly like the other station going quiet.
- **Bandwidth stayed on a CW filter after switching CW → LSB.** *(Samuel W7STF)*
- **The out-of-band tuner now exists in the browser too.** *(Samuel W7STF)* Out of band the Tab5 turns the band strip into a centre-detented coarse tuner; the browser simply hid it, so the one place you most want a way back to a band had no control at all.
- **Browser stalls — a real cause, and it was not your PC.** *(Samuel W7STF)* The message that tells a displaced browser "another browser took the live view" was a malformed frame, so browsers hung up instead of reading it and took the view straight back. Measured: 16 tug-of-war takeovers in ten seconds, down to 2 in twenty-five.
- **The frequency readout could stick** on an old value while the spectrum, waterfall and radio were all correct.
- **Radio Menus / Diagnostics colour is not fixed yet, deliberately.** The colours the menus use are all handled; the Diagnostics screen sends something else. Rather than guess, the firmware now reports exactly which codes it did not understand — if you can open Diagnostics and send that reading, it is the fix.


### v1.8.7 — 2026-08-19
**The browser panadapter stops freezing, and your logs can go to your own Cloudlog.**

- **The web panadapter no longer hangs for seconds at a time.** Reported as getting worse with every release, and it was. Measured over 9.6 hours, the browser session was being torn down **545 times** — roughly every 14 seconds in bursts — each costing about **2.2 seconds** of frozen display while the browser reconnected. Between the drops the stream ran at full speed, which is why it looked like stalling rather than slowness. The cause is a partial WebSocket write being reported as a complete one: a half-sent frame corrupted the stream, and the browser hung up. The background feeds only made it more likely, which is exactly why it worsened as more feeds were added. *(Samuel W7STF)*
- **Upload to your own Cloudlog or Wavelog.** The fourth logbook, and the only one you host yourself. Plain `http://` is allowed when the server is on the same network as the Tab5, so a home server needs no certificate — checked at every upload rather than once at setup, so it refuses from a field site instead of sending your API key across a network you do not control. Use `https://` for anything remote or for a hostname. Records go in batches and Cloudlog does its own duplicate checking. If your server is on your own network this is the only upload that needs no internet at all. *(Mark G4MEM)*
- **Radio menus show the radio's colours.** Everything rendered white while PuTTY showed the same screens in red and green. The colour was being read from the radio all along and simply thrown away by both the Tab5 and the browser. *(Samuel W7STF)*
- **The battery no longer reads 100% then 0% with no pack fitted.** Running from USB-C with no NP-F550, the display alternated between the two extremes. The detector for a missing battery was flapping in time with the supply rather than latching. *(Randy N4OPI)*
- **Waking from the screensaver no longer acts on the tap that woke it.** Sleep switched off the mouse pointer and left the touchscreen live. *(Randy N4OPI)*
- **A radio left receiving on VFO B is put back on A, and tells you.** Frequency changes only ever go to VFO A, so with the radio on B nothing you tuned had any effect — while band select still worked, which is what makes it look like your own mistake. *(Markus DL8MBY)*
- **The browser's spot and frequency labels are readable again** on a high-resolution display, where they were being drawn at about half size. *(Randy N4OPI)*
- **A warning that blamed the wrong thing.** After a cable swap you could be told to set the radio to two USB serial ports when it already was. The port is now retried, and the setting is only mentioned when the radio is definitely connected. *(Samuel W7STF)*
- **Pick callers myself.** New option in the FT8 Options modal. While you are calling CQ, a station answering does not start the exchange - they wait in the pile-up until you tap them, and the exchange then runs itself as usual. Only the choice becomes manual. It keeps calling CQ while you pick, and it overrides Auto-work pileup. Off unless you turn it on. *(Eric K3FNB)*
- **A Bluetooth mouse whose pointer moved erratically.** Connected and scrolled perfectly, but the pointer jumped about. The mouse fix in v1.8.4 went into the USB path and this mouse is Bluetooth, so it never applied; and the movement itself was being read with the wrong layout, turning a small movement into a large jump the wrong way. Diagnosed entirely from the diagnostic log that was sent in. *(Kevin KW6E)*
- **Entering FT8 could reboot the device.** A shared decoder buffer could be released twice if the decoder was set up twice without being torn down in between. Caught on the bench while testing this release, and almost certainly the unexplained heap crash that had been on the list since v1.3.0.
- **Under the surface:** a crash after about seven hours of healthy operation is fixed, the flash-persisted diagnostic log no longer stops writing with space still free, and two silent USB workarounds now count what they catch so it is possible to tell "never happened" from "happened and was handled".

**Investigated, no change:** the first entry into Radio menus after a flash drawing blank could not be reproduced — the first open after flashing plus six more were all clean, and the same thing is seen in PuTTY, which points at the radio's own redraw. *(Samuel W7STF, Randy N4OPI)*


### v1.8.6 — 2026-08-18

**A same-day fix release. v1.8.5 shipped with the browser interface completely dead.**

- **The web UI works again.** One broken text string stopped the entire page script running, so the browser drew its controls and then did nothing at all — no spectrum, no waterfall, no working buttons, "disconnected" in the corner, in both Chrome and Firefox. If you use the browser at all, v1.8.5 gave you nothing. The build now refuses to compile a page whose script does not parse, so this particular mistake cannot ship again. *(Randy N4OPI, Michael KZ4LY)*
- **A crash that looked like a radio fault.** An overnight test of v1.8.5 aborted inside the USB driver after about two hours of healthy operation. The reboot is not the expensive part: it happens with the radio still plugged in, which is the one situation that leaves the QMX unable to reconnect — so the radio then stayed dead until morning. The report was "the QMX wedged during the night"; the QMX was fine. Now the driver reports the error instead of restarting.
- **CW: the displayed frequency and tap-to-tune are corrected.** A signal transmitted on 7.060.000 appeared at 7.060.040, and tapping it tuned you 40 Hz off, so the other station heard you shifted. Two things added up: a calibration figure that has defaulted to the wrong value since before the panadapter read your CW offset from the radio, and the display rounding to whole analysis bins (47 Hz each). ⚠ Corrected by calculation that matches the reported measurements exactly, but not yet confirmed on the air — please measure and report. *(Roy KI0ER)*
- **Radio menus: you can see what you are typing.** Editing messages past number 9 meant typing blind behind the on-screen keyboard; the screen now scrolls so the radio's cursor stays visible. The help for a radio with no second serial port now tells you to power-cycle it. And the **two-finger tap to blank the screen** works properly instead of about one attempt in ten — two fingers never leave the glass at the same instant, and the one left behind was being treated as a deliberate touch. *(Michael KZ4LY)*
- **Also documented:** in the band config table, Enable/Disable entries accept **E** and **D** typed directly, and the arrows change the value rather than moving between columns — step onto a numeric column first to move across. That is the radio's own behaviour. *(Stan Dye KC7XE)*

**Not a bug:** auto-answer is off after every restart by design — a radio that started transmitting the moment the Tab5 powered on might be feeding an untuned antenna. WSJT-X arms transmit per startup for the same reason. *(asked by Brian WA6JFK, answered by Roy KI0ER)*

### v1.8.5 — 2026-08-17

**Mostly the things people had already been told were fixed, plus two found on the bench.**

Six of these were described as done in replies sent before v1.8.4 shipped, so anyone who believed those replies went looking for them in a build that did not contain them. That is the main reason this release exists.

- **Radio menus: you can see what you are typing, and type.** A **cursor** is drawn — it was tracked internally all along and simply never shown. **BS deletes leftward**: land on a value, backspace away what you don't want and type the rest. There is an **on-screen QWERTY** on a keyboard button in the top row, which matters on a Tab5 without the snap-on keyboard — there was otherwise no way to enter a value at all. **"Exit terminal" no longer re-opens the session**, and when the radio has **no second serial port the full menu path is on screen** — `System config → GPS & Ser. ports → USB serial ports → 2` — instead of a message that disappears. Values longer than two digits, and values in tables, are backspace-and-retype rather than arrow-adjust; that is how the radio has always behaved. *(Randy N4OPI, Michael KZ4LY)*
- **The web log viewer can correct a report.** It is called *View / edit log* and could only delete. The two report columns are now click-to-edit, and leaving one empty records that no report was exchanged. Only the reports can change — callsign, band, mode, date and time are what QRZ, eQSL and LoTW match a contact on. An edit corrects the Tab5's log only; a copy a logbook already holds cannot be amended by re-uploading. Related: a report is now **logged only if it was actually transmitted**, where before an armed message that got replaced could still reach the log. *(Gyula HA3HZ)*
- **The station you are working never disappears from the list.** With *Show only CQ callers* on, your own exchange vanished — two filters were stacking. The contact in progress is now exempt from all of them. *(Roy KI0ER)*
- **Leaving Radio menus hands the radio back properly.** After a menu visit the waterfall could misbehave until you paused and resumed by hand; closing the terminal now does that for you. A QMX's IQ mode is session state that a trip through its own menus can drop. *(Roy KI0ER)*
- **A pileup caller is answered from what they actually sent** rather than always a bare report — the last of a family of bugs where the manual **Transmit** button was right and the automatic path lagged.
- **The clock no longer says `UTC(GPS)` on a radio with no GPS.** The Tab5 decided a radio was GPS-disciplined when its second-tick agreed closely with internet time — but the Tab5 also *sets* that clock on a radio without GPS, so it was reading back its own handiwork. Measured across four pushes, the agreement landed anywhere from 12 ms to 834 ms, so the same firmware would say GPS on one unit and not on another for no reason at all. It was not only a wrong label: once it believed there was GPS it **stopped keeping that radio's clock right**, and a QMX loses its clock every time it is switched off. A clock the Tab5 set is no longer treated as evidence about itself.
- **In CW the display follows the offset you actually set.** The dial agreed with the radio but the waterfall did not, and tapping a signal tuned you about 30 Hz off — so you transmitted off frequency as though XIT were on. The radio's CW offset was read **once**, when the Tab5 connected, and never again, so changing it on the radio left the display correcting by the old figure for the rest of the session. It is now re-read every few seconds while you are in CW. *(Roy KI0ER)*
- **A caller who answers your CQ with a report instead of a grid is followed properly.** Someone who already knows they have you often skips the grid and reports you straight away; the reply should acknowledge that with `R` plus your report, not send another bare report and lose a cycle. Tapping **Transmit** by hand always did this correctly — it was only the automatic run that was a step behind. *(Gyula HA3HZ)*
- **A dated, day-at-a-time ADIF export.** A **Today only, dated file** link beside the ADIF download gives just that day's contacts, named `qso-YYYY-MM-DD.adi`, so a daily file already says which day it is. *(Gyula HA3HZ)*
- **The red transmitting banner no longer covers the text under it.** The panel was 99 pixels tall and holding 406 pixels of content with nothing to contain it. It can now shrink and scroll, and the status line no longer wraps onto three lines. *(Gyula HA3HZ)*

**Known limitation, unchanged:** in Radio menus, values longer than two digits and values inside a table still do not change with the ◀ ▶ keys — Max PA Voltage, the band-config columns, CAT timeout, TCXO and the Virtual U3S fields. They need some key other than left/right, and the question has gone to QRP Labs rather than a guess going into a release. Everything else in the menus edits normally.

### v1.8.4 — 2026-08-16

**Your radio's own menus on the Tab5, and a batch of fixes that stop it doing things you did not ask for.**

- **The QMX's own menu system, on the Tab5 and in the browser.** Settings → Radio → **Radio menus** shows the radio's 80×24 menu screen with arrow keys, Enter and Back. For a **QMX+ with no control panel this is the only way in** — everything the front panel would reach, including Band config and System config. It runs on the radio's **second** USB serial port, so the panadapter keeps decoding while you are in the menus; enable that once on the radio under System config → GPS & Ser. ports → USB serial ports → 2. Closing walks the radio back out through its own *Exit terminal* item, and if you close the browser tab or leave it two minutes it hands the radio back by itself. *(Randy N4OPI, seconded by Michael KZ4LY)*
- **Auto-answer now stands down when you would not expect it to be running.** It waits until it has heard **both** transmit windows before its first call, instead of picking a frequency from nothing after a band change. **Cancelling a transmission switches it off** — so halting a transmission to go and check your antenna does what you expect, rather than the radio starting again a cycle later. **A band change switches it off**, whichever way you changed band, because the antenna is usually not tuned for the new one yet. And **it is off at every startup**. *(Roy KI0ER)*
- **A transmit offset you choose during a QSO is used.** It was refused whenever a burst happened to be on the air — which, since a transmission fills most of every other slot, was about four attempts in ten. The exchange then carried on at the offset it started with, exactly when you were trying to move out from under someone. It is now accepted immediately and applied the moment the burst ends. *(Roy KI0ER)*
- **Spur suppression offers the setting that works first.** Both were there, but the weaker one was offered first: measured on 20 m, **Erase spur bins** takes the spur columns down about 78% against **Subtract**'s 28%. Erase now comes first. It does not leave dark holes — it ramps between the neighbouring bins — and what it learns is remembered per frequency, so the two-second measurement happens once. *(Samuel W7STF, who reported it as not seeming effective, and was right.)*
- **A USB mouse is read from its own description instead of an assumption.** A mouse reporting 16-bit movement had it read as 8-bit, so the pointer flew sideways, jumped between points, and barely moved vertically. *(Kevin KW6E)*
- **A WiFi hiccup can no longer restart the Tab5.** The WiFi transport used to restart the whole device rather than drop a single frame — and because it was a clean restart there was no crash report, so it looked like a mystery. It now drops the frame and keeps going.
- **Smaller, all reported by users:** the **Close button in the QSO log is no longer red** — it was a brighter red than *Delete all*, which is exactly backwards *(Gyula HA3HZ)*; a **QSO that could not be saved is no longer reported as logged** *(found while answering Gyula's question about how much the log holds — about two thousand contacts)*; and the **top bar no longer gets stuck showing the wrong mode or band**.

### v1.8.3 — 2026-08-14

**A field-report release: every fix in it was found by someone using the radio.**

- **Your QRZ and eQSL logins can be changed from the browser.** The prompt only ever appeared when nothing was stored, so a key typed wrongly — or one the service later reissued — could not be replaced from the page at all, and the only ways out were editing the config file by hand or a full erase-and-reflash. There is now a **Change QRZ API key** and a **Change eQSL login** row under the upload links, which appear once something is stored. *(Brian WA6JFK)*
- **The dB scale labels follow the range you set.** They were fixed at −40 down to −120 and ignored your Min and Max completely, so any range other than the default was described by labels that did not belong to it. They are now worked out from whatever range you choose, with a finer step for a narrow range. At the default range they are unchanged. *(Samuel W7STF)*
- **The "Adaptive floor" slider has been removed.** It could not change anything — the per-bin noise floor it blends towards is re-seeded many times a second, so both ends of the slider produced the same picture. It was already absent from the browser; a control that cannot do anything is worse than a missing one. The stored value is kept, so it can return if the underlying tracking is ever fixed. *(Samuel W7STF asked why there were so many handles — this was the answer.)*
- **The filtered part of the spectrum is drawn where the radio actually filters.** There was a gap of about 250 Hz between the dial frequency and the start of the shaded passband, because that edge was a fixed number that never came from the radio. In the digital modes the QMX uses one fixed filter of **150 to 3200 Hz**, and it reports 3200 as the top edge rather than as a width — so the shading was drawn at 200 to 2900. Corrected, and measured on the screen afterwards. *(Samuel W7STF)*
- **RF gain no longer sticks on "reading…".** It displayed the answer to the *previous* question, so the first time you opened the drawer there was nothing to show and nothing repainted it when the answer arrived — it cleared only when you next opened the drawer, and a single unanswered query left it stuck for the whole session. Since the gain is stored per band, it came back on every band change. It also now says **"radio not connected"** when the radio is not there, instead of implying it is being read. *(Samuel W7STF)*
- **The dark bands at the edges of a zoomed view are about half as wide.** Zooming filters the signal before re-analysing it, and that filter began rolling off just inside the edge of what is drawn. The filter is now twice as long. Widening it instead was rejected deliberately: that would have traded the dark edge for false signals appearing where nothing is transmitting. *(Samuel W7STF, whose own estimate of the width was accurate.)*
- **Out of band, the band strip is now a coarse tuner.** Inside a band the strip is a map — where you touch is where you go. Outside one there is nothing to map, so a handle sits in the middle and you **drag it off centre to move the dial**; let go and it springs back. A full pull moves by half of what is on screen, so two drags overlap instead of skipping a gap, and it gets finer as you zoom in. *(Samuel W7STF, who rightly pointed out that a row saying only "out of band" earns nothing.)*
- **The browser's decode list shows distance and bearing.** KM and BRG columns after the audio tone, in the same order the Tab5 uses, switching to MI if you have *Distance in miles* ticked. The Tab5 works it out and sends it, so the two screens cannot disagree, and a station that has not sent a grid shows a dash rather than a made-up number. *(Tony Abbey)*

### v1.8.2 — 2026-08-13

**Your radio's own spurs can be removed from the display, and a POTA clock that stopped being stolen.**

- **The spurs your radio makes itself can now be taken off the display.** If you have ever seen evenly spaced signals that are always in the same place, do not move when you tune, and are still there with the antenna unplugged — those come from the QMX's own synthesizer. On the bench at 14.074 MHz, the FT8 calling frequency, the strongest sat nearly 40 dB above the noise floor. The panadapter finds them by nudging the dial 25 Hz for about two seconds: a real signal stays where it is, while these move sixteen to fifty times further. **Off by default** — Settings → Waterfall → Spur suppression, with **Subtract** (can never hide a real signal) or **Erase** (they disappear completely). Wherever something is being removed, the line under the frequency labels turns teal, so you can always see what is being touched.
- **A QMX without GPS no longer overwrites an accurate clock.** Set the Tab5's RTC at home, arrive at a POTA site, switch the radio on — and your accurate UTC was replaced by the radio's power-on 00:00, after which FT8 stopped decoding. The clock was only protected while the network time was fresh, and offline it never is. The Tab5's own RTC now wins, and it sets the radio's clock instead. *(Don WB0LQW)*
- **RIT can be parked instead of cleared.** **Long-press the RIT button** and the offset is remembered while RIT switches off; long-press again and it comes back unchanged. For a net or a round robin where one station is off frequency, the offset comes and goes as the turn passes, without re-dialling it. The button reads `RIT (+250)` while an offset is parked. *(Roy KI0ER)*
- **The RIT offset is shown on the waterfall**, beside its own marker, so you can read how far off you are listening without looking at the corner. *(Samuel W7STF)*
- **The band strip no longer vanishes when you are out of band.** It reads "Out of band" in one flat colour and comes back to normal as soon as you are inside a band, so the row is never just empty and the coarse-tune drag stays where your thumb expects it. *(Samuel W7STF)*
- **The Operator Identity window no longer appears for unrelated faults.** Calling CQ with a message that would not build sent you to check a callsign that was perfectly fine. The real error is now shown. *(Don WB0LQW)*
- **The panadapter no longer switches your radio off trying to fix something it cannot.** After certain restarts the QMX stops answering on USB until it is power-cycled. The recovery meant for a stuck USB port was firing at that and cutting the port's 5 V — switching the radio off in front of you, for nothing. It now recognises the difference and leaves the radio alone.
- **"Adaptive floor" is documented as having no effect.** Both ends of the slider produce the same picture. It was already absent from the browser for that reason; now the manual says so rather than describing a control that does nothing. *(Removed outright in v1.8.3.)*

### v1.8.1 — 2026-08-12

**The fixes from the first day of v1.8.0 in the field.**

- **The spectrum is tunable again wherever you can see it.** Most of the spectrum had stopped responding to tap-to-tune — only a window in the middle worked, so it felt as though tuning only worked near the centre frequency while the waterfall was fine. The touch areas behind the top bar had been made shallower without the tune code being told, leaving a strip that belonged to nobody. The rule now is simply that **if the mouse pointer is white, clicking tunes**, and a finger behaves the same.
- **CW centre reaches the value your radio actually uses.** The radio offers 500–950 Hz in 25 Hz steps and the slider offered 600–800 in 50 Hz steps. It also could never agree with the radio, because the Tab5 pushed its stored value about thirteen seconds before the CAT link exists — so it now **reads the centre from the radio** at connect. *(Samuel W7STF, Roy KI0ER)*
- **The CW transmit offset puts VFO B back.** Switching it off did return the radio to simplex — FT8 was never transmitting off frequency — but VFO B was left at your frequency plus the offset for the rest of the session. It is now restored and simplex is confirmed by reading it back. Your QMX may still *show* both VFOs; that is on the radio's side and is reported to QRP Labs. Switching band and back clears it. *(Roy KI0ER)*
- **RF gain and volume agree between the Tab5 and the browser.** Whichever screen you opened second used to show the value from before your change. *(Samuel W7STF)*
- **Browser spot labels no longer swallow clicks meant for a signal.** A callsign can be 3–4 kHz wide on screen, and clicking anywhere in it took you to that station. A label is now a target only close to the frequency it marks. *(Samuel W7STF)*
- **The seconds can be set again.** Hours and minutes were editable and the seconds were not, so with no WiFi and no GPS there was no way to get the clock inside the second FT8 needs. **Hold the SS box and release on the minute.** *(Don WB0LQW, gesture by Roy KI0ER)*
- **The RIT button can be hidden** — Settings → Radio → Show RIT button, on by default. An offset that is actually engaged still shows itself. *(Samuel W7STF)*
- **Saving settings from the browser could fail with "HTTP 400"** once the form grew past 1 KB.
- **The manual now says a Bluetooth mouse must be BLE 4.0 or later.** The Tab5's Bluetooth has no Classic radio, so an older Classic mouse never appears at all and no firmware change can help. *(Roy KI0ER)*
- **Bluetooth mouse decoding is unchanged this release.** The real fault was found — only the first 22 bytes of the layout description every mouse publishes were being read — but the fix broke something else and was reverted. It will be redone.

### v1.8.0 — 2026-08-11

**RIT you set by tapping, summit spots, and a browser that finally matches the Tab5.**

- **RIT — receive off your transmit frequency.** A caller answering slightly off your frequency can be pulled in without moving transmit. Tap **RIT** at the top right of the spectrum to arm it, then tap the caller: a dashed magenta marker shows where you are listening, in the spectrum and down the waterfall, while the gold line goes on meaning the dial — and therefore transmit. The filter window moves onto the caller too. It stays armed, so the next caller is another tap, and retuning clears it. Works from the browser as well. *(Roy KI0ER, shaped with Michael KZ4LY and Bill Carver)*
- **SOTA spots.** Summit activations join POTA, RBN and the DX cluster on the spectrum, from [spothole.app](https://spothole.app) with Ian Renton M0TRT's permission. Off by default — it is a volunteer-run server.
- **Fox/Hound (DXpedition) mode, hound side.** Off / Guided / Automatic. Calls from above 1000 Hz, moves onto the Fox's frequency when answered, and stops on its `RR73`. **Simulation-verified only — no real DXpedition has seen it yet.**
- **The browser caught up with the Tab5, control by control.** RIT; starting and stopping a POTA/SOTA activation, with a badge while one is running so it cannot be forgotten; CW pitch, IF calibration, the battery charge limit, the 180° screen flip, Fox/Hound, simulation mode and the spot mode filter; and a "Prepare for flashing" item. Only the clock-sync window is still Tab5-only.
- **The browser's spectrum and waterfall now look like the Tab5's** rather than approximating them — same colour maps, same floor arithmetic, and the black level, contrast, colour scheme and smoothing settings finally reach it.
- **Spot mode filter.** Hides spots you cannot work in the mode you are in, since tapping a spot sets the mode as well as the frequency. *(Michael KZ4LY)*
- **Mouse and pointer.** A proper arrow that turns bright green over anything a click would act on, clickable edge grips, and tappable handles on the drawer and Memory Channels.
- **CW transmit offset narrowed to ±300 Hz**, and the guidance corrected: earlier releases suggested 400–600 Hz, which is outside many operators' filters. Around 100 Hz or less is what works.

⚠ **Two things to check if you use the browser.** **SWR protection set from the browser was never saved** on any v1.7.x build — check it on the Tab5 (Settings → "SWR protection (transmit)") if you set it there. And **spot labels were stealing clicks**: a callsign's clickable area was wide enough to swallow several kHz, so clicking a signal near one tuned you to that station instead. Both fixed, along with **Bluetooth mice that send a different byte layout** than the one on the bench, and **two browsers fighting over the live spectrum**.



**v1.7.2** — 2026-08-10 — bug-fix patch

- A Bluetooth mouse could stop reconnecting until you restarted the Tab5: a part-way failed attempt stopped it trying again, and the mouse sleeps after about half a minute, so it happened routinely.

**v1.7.1** — 2026-08-10 — bug-fix patch

**Tried a Bluetooth mouse on v1.7.0? Install this so your WiFi doesn't go flaky.**

- **The Bluetooth mouse was making WiFi unstable.** Switching the mouse on could leave the web UI flapping between Connected and Disconnected, with the link dropping and recovering for as long as it stayed on. The two look unrelated, which is why it took days to find: WiFi and Bluetooth share one link to the wireless co-processor, and the Tab5 was listening for *every* Bluetooth device in the building, continuously — thousands of advertisements a minute in a busy room, all of it crowding WiFi off the same pipe. It now listens in short bursts, and once your mouse is paired the co-processor ignores everything else, so that traffic never reaches the Tab5 at all. On the bench the underlying link errors went from about five a minute to none, with the mouse still connecting by itself. **If you have not used a Bluetooth mouse, nothing changes for you.**
- **The settings drawer would not close.** The swipe only worked if your finger started on bare background, so it felt random. **Tap anywhere outside the drawer.** *(Michael KZ4LY)*
- **CW transmit offset by the hertz** — **−50 / −10 / +10 / +50** buttons under the slider, which covers 2000 Hz in two-pixel steps. Range deliberately unchanged. *(Michael KZ4LY)*
- **DX cluster spots stop vanishing.** A cluster with nobody typing was treated as a dead connection and dropped about every 70 seconds, losing its held spots. Quiet is normal on a cluster.
- **The browser said GPS with no radio attached** — the clock fault Don N2VGU reported, fixed on the Tab5 in v1.7.0 but missed in the web page.
- **A failed WiFi scan says why**, and one that never returns no longer sits on "Scanning..." forever.

**v1.7.0** — 2026-08-09

A mouse, the phone spots that were missing, and knowing who hears you.

- **A Bluetooth mouse, with the radio still plugged in.** A USB mouse cannot do this: the QMX occupies the Tab5's only USB port, and sharing it through a hub does not work on this hardware. A Bluetooth mouse never touches that port. Pair it once — it reconnects by itself afterwards, across reboots and updates — and you get a pointer, a left click, and a wheel that scrolls whatever is under it. For anyone whose hands are cold or unsteady, every control becomes a click instead of a precise tap on glass. A symbol in the bottom bar shows off, scanning or connected. Off by default; see [Settings](guide/settings.md).
- **DX cluster spots — the phone activity RBN cannot see.** RBN is automated skimmers, and no machine recognises a callsign spoken into a microphone, so every SSB station on the band was invisible to it. A cluster is people typing. Mode is worked out from the spotter's comment or your band plan, park and summit references are picked out of the text, and skimmer spots relayed onto the cluster are dropped so they cannot double what RBN already shows. See [Live Spots](guide/spots.md).
- **Activation mode for POTA and SOTA.** Start an activation and every logged contact carries the reference automatically, with the count shown against the threshold — ten for POTA, four for SOTA — read from the log itself so it survives a reboot. Chases are tagged too, from spots already on screen. The ADIF download can be limited to one reference, so you upload that park's log rather than your whole file. Stopping is as prominent as starting, because the mistake that actually happens is driving home with it still on.
- **SWR protection while transmitting.** The QMX reports SWR over the control link; above your limit — 3.0:1 by default — the transmission is cut short and the transmitter latched off until you clear it. An FT8 burst is nearly thirteen seconds of continuous key-down, so a disconnected or wrong-band antenna has real time to do damage.
- **Who is hearing me.** Asks PSK Reporter which receivers have copied *your* callsign, listing distance, bearing and the report they gave you. The valuable case is the mismatch: stations you can hear that cannot hear you is a transmit-side fault, and from the receiving side alone that looks exactly like a dead band.
- **Spots appear once.** An activator spotted on POTA *and* heard by RBN was drawn twice, in two colours, almost on top of each other; RBN also doubled itself where two skimmers rounded the same signal differently. One station is now one entry, and the RBN sighting is folded in as corroboration — meaning a receiver actually copied them just now, rather than a self-spot typed an hour ago.
- **Fixes:** the clock claimed GPS time with no radio attached (Don N2VGU) — the Tab5 has no GPS of its own, and it was trusting a remembered verdict; each spot source checkbox is now genuinely its own source, where switching off POTA used to blank the whole lane and leave its spots behind; a tune started from the web UI now says so on the Tab5; and Antenna Tune no longer cancels itself after a second while leaving the radio keyed.

**Not yet confirmed on the air:** SWR protection has never seen a real mismatch, and the DX cluster lane has been verified as a feed but not watched drawing on a busy band. Reports welcome.

### Installing v1.7.0

1. Use the one-click flasher from the [Releases page](https://github.com/SteffenLav/qmx-panadapter/releases)
2. Or follow [Build from Source](build/build.md)

Your settings are preserved during a normal flash.

**v1.6.0** — 2026-08-09 — the browser became a second operating position: reply to a station, pick your TX tone, edit settings and memories and read the manual from any browser; `qmx.local`; CW transmit offset, RF gain and Release radio; self-recovering audio; the manual gained an A-Z index and drawn diagrams; the settings drawer was grouped with a Basic/Expert toggle.

**v1.5.0** — 2026-08-06 — Context-sensitive help: the **User Manual** button opens the chapter for the screen you are on, warning banners are tappable, and a **Need guidance?** panel lists symptoms in plain words with the ones the device can see highlighted. Plus Call CQ from the browser (Dennis WN4FLA) and the station you are working held at the top of the decode list (Don WB0LQW).

**v1.4.0** — 2026-08-05 — **Live spots on the spectrum** (POTA, with RBN as an opt-in second source): callsigns drawn on the trace at the frequency the station is actually using, **grey when you have already worked them on that band**, press-and-drag to pick one and lift to tune it *with the right mode*; spots fade with age and go after 30 minutes, and corner counts take you to the ones just off-screen. Behind that, three long-standing causes of instability root-caused rather than patched around: **all internet uploads were failing** (QRZ, eQSL, LoTW and the update check — a hardware crypto engine starved of memory), **52 KB of internal memory was being held by the firmware's own tables** (low-water mark 0 KB → 32 KB — one cause covering the SD remount failures, USB not re-opening after a radio power-cycle, and reboots after an hour of FT8), and **power-cycling the QMX could freeze the Tab5** on the dead USB connection. Plus **WiFi remembers up to six networks** and moves between them itself (Roy KI0ER), four of Roy KI0ER's five FT8 findings fixed, and the **QRZ / eQSL / LoTW setup no longer hidden** until you have logged a contact (Brian WA6JFK).

**v1.3.6** — 2026-08-03 — A pure fixes release, two of them closing problems as old as the project: the **"restart the QMX again and again" USB mystery solved** as two separate bugs (the Tab5-side one fixed with self-healing; the QMX-side one detected and explained on screen, reported to QRP Labs), a **crash on radio power-on** fixed (Dennis WN4FLA), **WiFi Scan working away from home**, **live TX status on the web page**, and the **"Diag(saved)" download delivering the crash log** with an SD card inserted.

**v1.3.5** — 2026-07-31 — Don WB0LQW's three requests, Roy KI0ER's bug report and the drawer touch fix: **CQ stops calling after a limit you set** (long-press Call CQ → **CQ stop**, never/1-5/10, live "call 2 of 4" counter, one extra listening slot after the final call), the **QSO log opens in your browser** (QSO Logs → View / edit log — sortable columns, per-row delete, type-DELETE Delete-all) with **Delete all on the Tab5 too** (two-tap, "ALL 34?"), an escape for the **busy-station hold** (TAP TO CANCEL), the **settings drawer stops stealing your finger** mid-drag, **QMX volume verified identical to the radio's own LCD** and capped at 50 dB (Randy N4OPI), and a latent bug fixed before it bit anyone: clearing the log would have silently disabled all future QRZ/eQSL/LoTW uploads.

**v1.3.4** — 2026-07-29 — The same-day field reports on v1.3.3, almost all from Roy KI0ER: a partner who never decoded your final now gets it **re-sent** (up to three times in four minutes) instead of the Tab5 moving on without ever making their log, finishing by hand no longer writes **duplicate log entries**, the fabricated `599` **signal reports nobody sent** are gone from the ADIF log, and the **occupancy map is filtered by time window** — a tone busy only in the opposite slot no longer reads as busy for you. The **TX frequency became a permanent button** on the FT8 screen with a **live mini occupancy strip** under the slot countdown, **TX Hold** (WSJT-X's "Hold Tx Freq") pins your tone, the parity choice is one cycling `TXCQ ANY / EVEN / ODD` button, and WiFi strength in the bottom bar is a **fan icon** with the freed width going to the network name.

**v1.3.3** — 2026-07-29 — Your TX audio frequency became visible and adjustable for the first time: a tone picker with a **live occupancy strip** across the 200-2800 Hz window, drag-to-pick, ±50 Hz, **Find clear slot**, changeable mid-QSO but never mid-burst (Roy KI0ER). The Tab5 also **stopped calling stations already working someone else** — which additionally stopped a merely-popular station being grey-listed for it — an incoming **`RRR`** began closing a QSO like `RR73` (it used to deadlock), a **QMX volume** slider in dB matching the radio's own LCD arrived for control-panel-less QMX+ builds (Randy N4OPI), and **LoTW uploads gained US state and county**, whose absence had been costing US operators their Worked All States and county credit (found via Paul N8HM's CardSat).

**v1.3.2** — 2026-07-26 — Grid squares are logged again (almost every QSO was missing `GRIDSQUARE` — John W5JSS), **PSK Reporter spotting** on by default (**since confirmed working**: "QMX Panadapter v1.3.2" is listed under "Software in use" at pskreporter.info with spots from six stations — note the 5½-minute delay before the first report, and that PSK Reporter never acknowledges anything, so that page is the only place to check), the **User Manual built into the firmware** so it needs no WiFi and no SD card and the "Save offline" button is gone, an explicit **microSD backup contract** (continuous with WiFi off and a green dot; one complete backup per start-up with WiFi on and a yellow dot — insert the card *before* switching on), **grey-listing** for stations that never answer (opt-in), and **pileup replies** using the intelligent-Transmit laddering (both Roy KI0ER).

**v1.3.1** — 2026-07-24 — Two decode-list columns requested by Roy KI0ER (**DT**, the station's slot-timing offset, and **HZ**, its audio tone, with the country column compacted to a 3-letter code), a boot-diagnostics fix so ST7121 units report the hardware actually detected (Paul VE3PIK), and UI alignment polish (QMX prompt placement, FT8 left-pane grid, settings-drawer dropdowns and sliders).

**v1.3.0** — 2026-07-23

Smarter manual FT8 operation — built around field feedback from **Roy KI0ER** — plus a rebuilt practice simulator, USB mouse support, and a web file browser for the microSD card.

- **Intelligent Transmit.** Tapping a decoded station's **Transmit** now sends the correct *next* message for that exchange — their CQ gets your grid (or your report with Skip-TX1 on), their grid gets your report, their report gets your roger, and so on — the same behaviour as a WSJT-X double-click. You can run a whole QSO by hand, one tap per step, and it **logs to ADIF** like any other contact when you send the closing RR73/73.
- **Faster replies.** The mid-slot reply window is wider and hand-armed exchanges now land on the beat instead of a cycle late. A subtle bug that made every exchange step transmit twice in some cases is fixed.
- **Fast pounce (early decode) — new toggle, on by default.** Decodes appear *before* the slot boundary (the way WSJT-X works), so answering a fresh CQ can transmit in the very next slot instead of waiting 30 s. ⚠️ *Honest note: this specific feature hasn't been verified on a live band yet (no antenna at the development QTH until mid-August). The known trade-off is that a station transmitting late in the slot can occasionally be missed. If your decode counts drop with it on, switch it off in the FT8 settings drawer — and please report your before/after numbers on the groups.io thread; that's exactly the field data we need.*
- **Pileup no longer hides the log.** While callers are waiting the ADIF-log button reads "Pileup" — **holding it now always opens the ADIF log** (a one-time hint teaches the gesture). **Auto-work pileup** also starts draining immediately when you enable it with callers already waiting, and worked-before stations only vanish from the pileup when you've actually checked "Exclude worked-before".
- **Practice simulator, rebuilt.** FT8 Simulation Mode now needs **no radio at all** — six phantom stations (US + DX) call CQ, four of them answer your CQ at once (a real pileup to practice on), and they're patient like real operators: each repeats its message up to four times before giving up. They answer manual step-by-step Transmit, Auto Pounce, and CQ-runs alike. When you're done, a **"Del N test"** button in the ADIF viewer (only visible while practice contacts exist) wipes them from the log in two taps.
- **USB mouse.** Plug a mouse into the USB-A port — a cursor appears and clicks drive everything. *(Limitation: the mouse and QMX can't share the port, and a USB hub can't bridge them on this hardware — so it's for setup, log review and manual reading with the radio unplugged.)*
- **microSD file browser.** **Files → SD Files** in the web page opens a browser for the card — download logs and backups, upload, delete — without pulling the card out.
- **User Manual on a fresh boot** now waits for WiFi instead of asking for a reboot.

**v1.2.0** — 2026-07-20 — A built-in **User Manual on the Tab5 itself** (Settings drawer → User Manual): native markdown reader with a drag-to-pick two-column Contents page, offline copies to microSD (**Save offline**), and a quiet GitHub firmware-update check. *(The Save-offline mechanism was superseded in v1.3.2 — the manual now ships inside the firmware.)* Plus the FT8 pileup fix: a worked (or late-answering) station no longer lingers in the pileup. *(Thanks to Dirk DK7CVD.)*

**v1.1.0** — 2026-07-19 — A years-old FT8 decode mystery solved: the panadapter used to hear 60+ stations in its first slots then collapse to a fraction; the cause was ~200–350 ms of QMX audio lost at the USB wire every slot (invisible to every counter). Fixed — full decode rate every slot now. Plus the microSD card promoted to a full grab-and-go station backup (QSO log + settings + LoTW cert/key), automatic GPS time sync (~10 ms, no toggle), a band-plan drag from the bottom bar, and an ADIF-viewer crash fix.

**v1.0.1** — 2026-07-17 — Point fix: when you answer a CQ, the report you send back is your own measurement of their signal, not an echo of theirs. (Reported by Steve N0SZ, Jonathan KN6LFB.)

### v1.0.0 — 2026-07-16

**The 1.0.** The beta label is gone: the QMX Panadapter is a complete, self-contained FT8/FT4 station — receive, transmit, auto-QSO, logging, and upload to **all three major logbooks** — with no PC anywhere in the loop.

- **LoTW upload — the final piece.** QSOs are signed on the device with your own ARRL callsign certificate and uploaded directly to Logbook of the World. One-time guided setup from the web page (step-by-step TQSL certificate export, then import — your certificate passphrase never leaves your browser). Live-verified against lotw.arrl.org. Certificate renewal: Ctrl-click the LoTW button
- **The FT8 "everything sent twice" behaviour is fixed.** Replies used to miss their own slot by ~2 seconds and go out again next cycle, doubling QSO duration; the transmitter now holds ~2 seconds for the fresh reply and fires it on the same slot. Field-verified on air
- **Nonstandard callsigns work now**: special/compound calls (special-event stations, `PJ4/...`) no longer show as `<...>` once heard in full, and their answers to your CQ are recognized
- **Correct received reports in the log**: the partner's `R-06`-style roger is their real measurement of your signal and is now logged as such — no more 599 placeholders from CQ runs. (Old 599 rows can be deleted — see next)
- **QSO log viewer grows up**: a Today/All filter with a **POTA activation counter** (title turns green at 10 QSOs today), and **single-record delete** — long-press a row, drag to the right line, release, confirm. Deletions keep all three logbook upload positions consistent
- **Broken QSOs resume**: if a station fades mid-exchange and comes back within a few minutes, the exchange continues where it stopped (automatically, or by tapping their row) instead of restarting from scratch
- **Steadier decode list during CQ runs**: stations you can't currently hear (they transmit when you do) no longer vanish mid-run, and the list keeps entries for 2 minutes instead of 1
- **Display sleep**: set an idle timeout and the screen turns off while everything keeps running (FT8, radio link, web UI). Tap to wake — the wake tap can't press anything. Two-finger double-tap blanks immediately. A real battery win for unattended and web-only use
- **Reorganized settings drawer**: setup items (WiFi, callsign, band-plan region, brightness, battery care, display sleep) grouped at the top, display-tuning controls below
- **Web page polish**: the bottom bar's many buttons are now three tidy menus (QSO Logs / Files / Miscellaneous); a congested WiFi link can no longer freeze the page for seconds
- **A rare crash fixed**: a momentary USB glitch on the radio-control link could reboot the whole device; it now just retries
- Full writeup in [Version History Document](https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md)

### v0.21.0

- **FT4 returned** — the panadapter no longer redraws itself behind the FT8/FT4 screen, freeing the processor headroom FT4's 7.5-second cadence needs; verified end-to-end (RX + TX)
- Fixed a full-screen display flash (mostly in FT4); QRZ upload no longer gets stuck on already-logged contacts; reset settings or Wi-Fi from the web page; QMX VOX switched off automatically at link time

### v0.20.1 / v0.20.0

- **Robustness release.** WiFi "dies after a few minutes" fault now self-heals instead of needing a reboot; opening a window no longer freezes the device; the radio-control (CAT) link rides out USB glitches; the web UI pauses its stream while FT8 runs
- FT8 decode timing anchored to the exact slot boundary; FT8 pile-up list + "Skip TX1" quick pounce; 11 m/CB band; memory-channel overhaul; battery-care charge limit; web-UI whole-band plan strip, draggable split, better screenshots and keypad
- microSD now on its own bus (SD auto-archive still off — it squeezed memory needed by FT8)
- v0.20.1 hot-fix: fixed a reboot on every pounce (a scratch table overflowed a small task stack; moved to PSRAM)
- *(FT4 was switched off across v0.20.x for the memory pressure now resolved in v0.21.0.)*

### v0.19.5

- AM mode + Antenna Tune for QMX 1_04+ firmware (invisible on stable 1_03_002)
- Fixed a crash on leaving FT8 mode; WiFi on/off now applies live and no longer wipes a saved password; FT8/FT4 remembers its own frequency
- Band picker shows all bands in two columns; band-plan strip is a see-through framed window with 6 m segments; steadier point-to-tune; settings-drawer slider/scroll fixes; DiGi-gated memory recall in FT8/FT4

### v0.19.4

- FT4 usability — four stacked faults fixed: a capture buffer's FFT workspace in slow memory (every other slot decoded nothing), a mis-firing stuck-decoder watchdog wiping the list mid-QSO, an uncapped per-slot clock nudge, and slot-parity computed on FT8's grid in FT4
- QMX IQ-mode confirmation hardened (readback no longer fooled by the command echo)
- See-through frequency keypad; settings-drawer declutter (removed Snap-to-signal + FT8 sync lines, moved Band-plan region)

### v0.19.3

- QMX IQ-mode handshake retried automatically (up to 4 attempts), with a red on-screen banner if all attempts fail — a silent failure could leave a whole session without I/Q data (spectrum shifted, tunable across the full 48 kHz window)
- Band-plan strip: tracks zoom/pan live, shows the filter passband at band scale, drag-to-tune and tap-to-jump directly on the strip
- Memory channel drag-to-move; tap an empty slot to create; out-of-band frequencies rejected immediately
- Frequency entry popup: draggable, resizable (pinch/swipe), position remembered across reboots
- ADIF log viewer rebuilt: real column alignment, sticky header, Country and Mode columns, Sent/Rcvd split, zebra rows
- FT8/FT4 TX confirm dialog: up/down nudge buttons; FT4 countdown and title corrected for 7.5-second slots
- FT4 clock-sync fix (timing offset was using FT8's block geometry, ~3.3× wrong) and FT4 decode-quality fix (FT8's iteration cut no longer applied to FT4)
- QRZ/eQSL uploads more reliable (SD auto-archive paused during uploads); web UI stale-connection freeze capped at 5 s

### v0.19.2

- microSD auto-archive: diagnostic log, ADIF log, and a config export are mirrored automatically to a microSD card when one is inserted
- Diagnostic log is now always-on and survives a power loss (a rolling copy persists to internal flash, downloadable even with no SD card)
- USB reconnect fix: power-cycling the QMX after WiFi is up no longer breaks audio/CAT
- Crash fix: a USB disconnect race that could reboot the Tab5 when the QMX dropped off USB
- QMX IQ mode is now verified, not assumed — the panadapter checks the radio actually accepted the I/Q-mode command instead of just checking the USB write succeeded
- FT8 continuation messages (report, RR73, 73) resend less often during an active QSO — decode time on busy slots cut by ~25-30%

### v0.19.1

- New project homepage at [tab5.lav.dk](https://tab5.lav.dk) — the user guide and reference as plain web pages
- Logbook uploads (QRZ / eQSL) and log downloads now work reliably while FT8 is running — no more reboots or dropped WiFi during a transfer

### v0.19.0

- FT4 transmit and receive (7.5-second slots, 105 symbols, 48 ms cadence) — CAT cadence verified on real QMX hardware
- Per-mode sticky frequency/bandwidth/filter recall between FT8 and FT4

### v0.18.8

- ARRL Field Day FT8 exchange mode (class + section in message, special TX sequence, ADIF logging)
- FT8 simulation mode (practice QSOs with phantom stations, no radio keyed)
- FT8 decode yield investigation closed (found and fixed 3 separate CPU contention issues)

### v0.18.7

- Decode-yield gap **closed** (controlled A/B with v0.18.0 confirmed fixes sufficient)
- Auto-answer robot mode un-shelved (live TX, full disclaimer on-screen)
- CQ tone auto-relocation on clash
- SNTP/QMX time-priority fix + FT8-derived auto-sync
- Flasher recovery mode + auto port detection

### v0.18.6

- FT8 sparse-decode investigation: found 3 separate regressions since v0.18.0
- Fixed: `cw_audio_init()` ghost task, missing poll-task CDC tolerance, waterfall style-set loop
- RST_SENT fix in CQ-run QSO
- Distance-in-miles display toggle
- Diag-log dot + firmware version in bottom bar

### v0.18.5

- Band-aid for FT8 decode regression (incomplete; issue fully addressed in v0.18.6)
- Critical bootloader-corruption hotfix (flasher wrote to 0x0 instead of 0x10000)
- FT8 double-spawn crash guard restored

### v0.18.4

- Band-nav strip (CW/Digi/Phone color zones below frequency axis)
- One-finger pan/stroll (spectrum scroll + center-freq readout)
- Snap-to-signal peak detection
- Band-aware worked-before (callsign + band memory)
- Robot mode (complete but **shelved** — greyed out, disabled)

### v0.18.3

- Waterfall live controls (black level, contrast, adaptive floor, FFT window selector)
- Display 180° flip toggle
- Image rejection investigation (reverted — not a firmware issue)

### v0.18.2

- Idle reboot resolved (WiFi SDIO RX streaming → mempool recycled buffer)
- Web UI freeze/reconnect fix
- BW from web (CW passband)

### v0.18.1

- Config backup/restore (settings + memory channels + ADIF log as INI file)
- Flasher clean-flash option
- Memory recall fix (CAT race condition → optimistic display + deferred writes)
- Fast Panadapter↔FT8 toggle crash fix

### v0.18.0

- FT8 decode rework: streaming STFT + dual-core + reply-on-immediate-slot
- dBm scale restored on spectrum
- Multiple field-reported fixes (memory freq, tap-to-tune reversal, band lockout)

### v0.15.x and Earlier

See [Full Version History](https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md) for detailed notes on all earlier releases.

## Roadmap

### Next up (post-v1.0.0)

1. **Web-UI audio streaming** — listen to the receiver in any browser on your LAN, demodulated on the Tab5. Already working in development; held for quality tuning and an overnight streaming soak
2. **CW page** — canned-message CW TX memories, then decoded-CW display
3. **Binaural CW audio** — a stereo sound stage so two stations a few tens of hertz apart land in different places in your head (asked for by Roy KI0ER; shaped by Don N2VGU and Michael KZ4LY, whose point that the stage **width** should be a setting rather than a fixed angle is now the plan). Waits on the same audio-output rework as the CW page
4. **Live microSD mirroring while WiFi is up** — continuous mirroring currently only runs with WiFi off; v1.3.2 made that explicit and reliable (one complete backup per start-up with WiFi on). This was believed to be bus contention between the card and the WiFi co-processor. v1.4.0 found that a large part of it was actually a memory shortage — the pool the card needs to mount had been squeezed to almost nothing, and now has room again — so this may already behave better than documented. Needs a retest before the behaviour is changed

### Future Additions

- CW audio (shelved since v0.18.5 due to CPU contention; needs pipeline redesign)
- Offline maps (grid squares, distance visualization)
- Video tutorials & regional quick-start guides

## Download & Documentation

- **Source code:** [GitHub Repository](https://github.com/SteffenLav/qmx-panadapter)
- **Releases:** [GitHub Releases](https://github.com/SteffenLav/qmx-panadapter/releases)
- **User Guide:** [PDF](QMX-Panadapter-UserGuide-v1.11.3.pdf) or [Web](quick-start.md)
- **Build Guide:** [Build from Source](build/build.md)
- **Technical Details:** [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md)

---

**Have a question?** Check [Quick Start](quick-start.md) or [Troubleshooting](reference/troubleshooting.md).
