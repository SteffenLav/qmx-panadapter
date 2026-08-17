# Replies — 2026-08-17 batch (drafts, NOT posted)

Plain text. Copy the block under each name.

---

## 1. Roy Ashkenaz K2RMA — spurs on the QMX but not the QMX+

Most likely nothing is wrong with either radio: it depends heavily on where you were tuned.

What I measured on my own QMX, with the antenna disconnected so nothing external is involved: the spurs come in bad *windows* only 1 to 3 kHz wide, roughly six of them per 100 kHz of band. Inside one the comb is strong — teeth every 8015.6 Hz, the worst nearly 40 dB over the noise floor. A few kHz away there is nothing to see at all. 14.074, the FT8 calling frequency, happens to sit inside the only bad window in a 20 kHz stretch, which is why it looks like a QMX-wide problem when it is really a frequency-specific one.

Their position also moves with the dial, and fast — the offset shifts 16 to 50 times faster than you tune. So two radios that were not on exactly the same frequency have no reason to show the same picture, and neither does the same radio at two nearby frequencies.

So before concluding your QMX+ is cleaner: what frequency was each one on? If you compare them on the *same* frequency, with the antenna off on both, and one shows the comb and the other does not, that is a real difference worth knowing about and I would like to hear it. If they were on different frequencies, that alone probably explains it.

Two things worth knowing either way. The noise floor itself is 5-6 dB worse inside a bad window, so it is not only the visible teeth that cost you. And Spur suppression (Settings → Waterfall) is off by default; if you turn it on, take **Erase spur bins** — measured, it removes about 78% of the comb against Subtract's 28%, and it does not punch dark holes in the display.

---

## 2. Gyula HA3HZ — the four observations

Thank you — these were all useful, and two of them were real bugs.

**The red transmitting frame covering the text below it: fixed.** Measured in a browser rather than eyeballed, and it was worse than it looked: the panel is 99 pixels tall and the content had grown to 406, with nothing to contain it, so it spilled over its neighbours. Two causes, both fixed — the panel can now shrink and scroll instead of overflowing, and the status line no longer wraps to three lines. If you still have that screenshot I would like to see it, to be sure I fixed the one you saw.

**A caller who answers your CQ with a report instead of a grid: real, and not yet fixed.** You are right about what should happen — if he skips the grid and sends a report, your reply should acknowledge it (R plus the report), not send another bare report. It is on the list. It only shows up with operators who skip a step, which is why it survived this long.

**The dash in the report column, and not being able to edit it:** the dash is deliberate — it means no report was ever exchanged. The panadapter used to write "599" into those, which is a made-up measurement that then gets uploaded to QRZ and LoTW as if it were real, so that was removed. But you are right that if the field looks editable it must actually edit, and the log viewer letting you fix a value by hand is a fair thing to want. Also on the list.

**Daily ADIF download with the date in the filename: done.** There is now a "Today only, dated file" link under the ADIF download, giving you just that day's contacts in a file named qso-YYYY-MM-DD.adi — which should drop straight into your cqrlog routine.

One note on your reports: you mentioned these were seen on 1.8.2. Two of them I have reproduced and fixed against the current version, but if any of the four look different on 1.8.4 please say, because that changes where I look.

---

## 3. Randy N4OPI — the terminal editing list

That is an exceptionally useful list, and your own conclusion was the lead I worked from: *"numerical values in a table or those that are over 2 digits don't Incr-Decr properly."*

Three of the things you found were mine and are fixed:

**The cursor is now drawn.** It was being tracked internally and simply never rendered, which is why you could not see where you were typing in Messages.

**Backspace and Delete.** I was only ever sending one byte for both (0x08). A terminal application can want either that or DEL (0x7F), and the QMX manual does not say which — so there are now two separate keys, BS and DEL, sending their own byte each, and in the browser your real Backspace and Delete keys are wired to one each. **Please tell me which one actually deletes** — then I will make that the single sensible key and drop the other.

**Exiting from inside the radio's menu no longer re-opens the terminal.** Michael hit this too. It was a recovery I added for a lost startup character: choosing "Exit terminal" clears the screen, my code saw a blank screen and helpfully sent a character to wake it up, which put you straight back in. It is now limited to the first few seconds after opening, where it belongs.

**What I have not fixed, because I will not guess it:** the fields that refuse to increment — Max PA Voltage, the band config table columns, CAT timeout, TCXO, the U3S values. Your pattern says these need something other than left/right, and I would rather find out which key the radio actually wants by trying it on the radio than ship a guess into a release. I have the hardware this evening. If you happen to discover the right key first, that would save me the hunt.

The QWERTY overlay is on the list too, and I am glad you would use it. It waits behind the cursor and delete work — a keyboard is not much use typing into a field you cannot see.

---

## 4. Michael KZ4LY — both of your points

**The menu path is now on screen.** You put your finger on exactly the right thing: the person who needs that instruction is the one who never read the announcement. If the radio has no second port, both the Tab5 and the browser now show the full path — System config → GPS & Ser. ports → USB serial ports → 2 — laid out to be followed while you are looking at the radio, instead of a toast that disappears.

**"Exit terminal" firing back up was my bug**, and your guess about the cause was better than you knew. See Randy's block above for the mechanism. On your wider question — whether a clean exit from within the menu ought to close the session rather than leave it open — I think you are right, and that is the better behaviour. It is now at least harmless rather than actively unhelpful.

**The QWERTY overlay** is a good idea and Randy confirmed he would use it on his 12 oz build, so it is on the list.

And thank you for the earlier push on feasibility. You were right that the escape handling was the small part — the screen model is about 170 lines and host-tested against the actual bytes the radio sends. The part that took the work was everything around it: making sure a session can never take CAT down, and that closing always walks the radio back out of terminal mode.

---

## 5. Samuel W7STF — sub-1.838 MHz and the OOB slider

Taking your advice and waiting on the band-definition question until that thread settles — no point building to a moving target.

Two parts of it are ours regardless of how the definition lands, and they are on the list: the out-of-band slider **disappearing in the browser while it exists on the Tab5** is simply an inconsistency between the two screens, and the Tab5's own slider misbehaving below 1.838 MHz is a bug wherever the band edges end up. The 200 m band showing as 160 m in the top-left is a third, separate thing.

Good to know direct frequency entry gets you to 530 kHz. I have no broadcast that low here to test against, so anything you can tell me about what the display does down there is worth more than what I can measure.

And thank you for the terminal report — the Band Configuration problems are the same family Randy documented in detail, so they are covered by the same work.
