# Draft replies, 2026-08-05 (one per person)

Working drafts for the operator to edit and post. Not published by anything.

---

## To Brian WA6JFK

Brian,

Two separate things there, and one of them was our bug.

**Finding the QRZ setup.** You were right to be confused - the "QSO Logs" menu in
the web interface only appeared once you had logged a contact, and that menu is
the only way to reach the QRZ, eQSL and LoTW setup. So on a freshly flashed unit
there was no way to configure uploads before operating. That is backwards and it
is fixed in the next release: the menu is always there.

**Where each thing is set up.** WiFi, callsign and grid are set on the Tab5 itself
(swipe in from the right edge for Settings). QRZ, eQSL and LoTW are set up in the
web interface, because those need an API key or a certificate file and typing
those on glass is painful.

**Upload errors.** Two possible causes, and the log will say which:

1. A CLEAN install erases the whole chip, including the LoTW certificate and
   private key and your QRZ API key. Those have to be entered again. The normal
   (non-clean) update keeps them.
2. There is a real firmware fault we found this week that stops *all* HTTPS from
   the device, so QRZ, eQSL, LoTW and the update check all fail together. It is
   root-caused and fixed for the next release.

If you can send the log - web interface, "Diag(saved)" - I can tell you which one
you hit. If it contains "ctr_drbg_seed" it was fault 2, and the fix is already in
hand.

Re-entering WiFi, callsign and location after a clean install is expected - a
clean install wipes everything by design. The flasher warns about it, but the
warning is easy to miss.

73
Steffen

---

## To Dennis WN4FLA

Dennis,

Thank you for the three log files - they were exactly what was needed, and they
turned up more than the crash you reported.

**Your logs show three separate things:**

1. **The QMX itself returned a bad USB descriptor** ("8 bytes, expected 16"). That
   is a QMX firmware fault, not the Tab5, and it is the one I have reported to
   Hans. The "Check QMX+ USB or power" message you saw on the laptop is v1.3.6
   correctly detecting it. Nothing wrong on our side there.

2. **The Tab5 was running very short of internal memory** - your unit shows the
   same numbers as mine did. This is what made it reboot. I found the cause this
   week: 52 KB of internal memory was being held by our own code where it did not
   need to be. That is now recovered, and the free-memory figure went from about
   20 KB to about 80 KB.

3. **The lock-up after switching the radio off and on was a different fault
   again.** When you power-cycle the QMX the audio side sometimes gets no
   disconnect notification, and it then polled the dead connection flat out -
   which saturated one CPU core, froze the screen, and left nothing able to
   recover. That is why it took you three tries. Found, fixed and verified here:
   power-cycling the radio now reconnects immediately.

So your report covered two genuine Tab5 bugs and one QMX bug. The "blue screen
flash" you saw is a known display artefact, not a crash - it happens when the
display interrupt is delayed, and it is harmless.

Glad the TX status on the web interface is useful - that was the intent, exactly
the use you describe.

When the next release is out, please give it the same test: an hour of FT8, then
switch the radio off and on. If anything still misbehaves, the saved log again
would help.

73
Steffen

---

## To Roy KI0ER

Roy,

Thanks - detailed and specific, and four of the five turned out to be real. Point
by point.

**1. Station keeps sending R-xx after we send RR73.** Real, and two things were
wrong. There is already logic to re-send the final if the other station is still
asking, but it gave up after 3 attempts and then went back to calling CQ - over
the top of him, which is what you saw. It now re-sends up to 6 times over 5
minutes, and once that is used up it stays SILENT instead of calling CQ while he
is still calling us. Your suggestion, and the better half of the fix - just
raising the number would have delayed the same rude behaviour.

**2. Decodes drying up after an hour, strip going fully green, restart cures it.**
Not yet explained, and I do not want to guess. But your unit was running with very
little internal memory free (about 20 KB, and the low-water mark touching zero),
which makes allocation failures likely under load. I have recovered 52 KB of that
this week. Please retest on the next release - if it still happens, that tells me
the memory was not the cause and I will look elsewhere.

**3. What the offset strip is showing.** It is filtered to YOUR transmit window
when that is known, and shows both windows when it is not - which is exactly the
ambiguity you describe. Nothing on screen said so. The strip now carries an
EVEN / ODD / BOTH tag, and the tone picker says which window it is describing.

**4. Re-learning the free offsets during auto modes.** Two parts:
   - The strip going stale was a real bug - see 5 below. With that fixed, occupancy
     self-corrects and the existing "relocate if the tone gets busy" check finally
     has an accurate picture to work from.
   - Alternating even/odd while hunting: also real, and the mechanism is worth
     explaining. A QSO takes a fixed number of slots, so finishing one tends to
     drop you back to idle on the same parity every time, and you can sit in one
     window for a long stretch - never sampling the other. After 3 pounces on one
     window it now gives up one slot so the next lands on the other.

**5. Strip going fully red, all slots taken, restart cures it.** Found it, and it
was a real bug. Rows on your own transmit parity have their expiry paused, because
while you transmit over a station's slot its silence tells you nothing. Correct in
principle - but there was no upper limit, and during a CQ run you are parity-locked
essentially all the time, so those rows were kept for the whole session. The
occupancy strip is built from that same list, filtered to your parity, i.e.
precisely the rows being kept. So it filled up and stayed full until you restarted.
There is now a 10-minute ceiling on that pause.

**WiFi - your multi-network request is done.** It remembers up to 6 networks that
have actually worked, most recent first. You never have to enter them anywhere:
every successful connection is remembered automatically. If the network it is set
to will not come up, it scans and switches to whichever remembered network is on
the air, within a few seconds. And when you pick a known network by hand from the
Scan list, its password is filled in for you.

The list is in the config file you can download and re-upload, under
`[wifi_known]`, so it is inspectable and restorable like everything else.

Tested here by connecting to a phone hotspot and then switching the hotspot off -
it moved itself back to the house network without being touched.

Good to hear there were no crashes in those three days.

73
Steffen
