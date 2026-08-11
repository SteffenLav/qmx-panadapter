# Draft replies, groups.io, 2026-08-11

**Three posts, the third optional. Nothing posts automatically.**

Post 2 first if you only post one — SWR protection set from the browser has never been
saved on any v1.7.x build, so anyone who set it there is transmitting without it.

Everything below is fixed on the bench and named as **v1.8.0** — no date given.

Still needs Samuel's diagnostic log: the **60m "0m"** label. That is the only item on
his list I could not get to the bottom of from here.

**Coverage of the whole thread since your last post.** Every technical item is answered:
Samuel's big v1.7.2 list (#176440), his tuning oddity (#176446), Roy's non-repro
(#176447) and Samuel's follow-up (#176451). Deliberately NOT answered, because there is
nothing for you to add: the Tab5 pricing and tariff sub-thread (Samuel, John K7JFW, Don
N2VGU, Randy N4OPI) — John K7JFW's "Stef's panadapter software has caused a run on the
Tab5" is a compliment, not a question. Post 3 is OPTIONAL: John Dusek asked about running
without a battery and Don and Randy both answered him correctly, so it is only worth
posting because you are the authority and there is one firmware detail they could not
give him.

---

## Post 1 — to Samuel W7STF (and Roy)

Samuel — your second message solved it, and it was not the tuning maths. Everything
below is fixed and will be in **v1.8.0**.

**The spot labels were stealing your clicks.** Clicking a callsign takes you to that
station — that part is deliberate, and the Tab5 does the same. But in the browser
the label's clickable area is as wide as the callsign and sits at the same height as the
trace. `OK/DL4ROB/P` is 93 pixels wide, which at the default zoom is 3.8 kHz of band
where every click went to that station instead of the frequency you clicked. So the VFO
landed on a plausible nearby number and you heard nothing, because you were a couple of
kHz away. On a 300 Hz filter that is silence.

That also explains the two odd parts. Clicking outside the passband first works because
moving the dial re-flows the label layout, so your next click lands clear of a label —
nothing to do with the passband. And Roy could not reproduce it because it depends on
whether a label happens to sit over the signal you are aiming at.

The Tab5 does not suffer from it because there the two gestures differ: a quarter-second
hold tunes a point, a quick tap takes a callsign. A browser has one click for both.

Fixed two ways. The label's click area no longer extends above and below the text, so a
click anywhere else in that column tunes where you clicked — that is your escape hatch
for a signal sitting behind a label. And hovering a label now shows you a marker at the
station's real frequency with its callsign, so you can see the click will move you
before you commit.

Two other things I found in that code while looking, both real, neither yours: in CW
every spot label was being drawn 700 Hz to the left of its own signal, and the browser
was rounding clicks to a different frequency grid than the Tab5 (500 Hz in SSB where the
Tab5 uses 250). Both fixed.

**Your mouse.** BLE mice do not agree on a byte layout, and the Tab5 was assuming one —
the one I captured from a Logitech here, which packs the movement into 12 bits. If yours
sends 16-bit movement, that assumption reads the vertical value four bits early and it
comes out about sixteen times too large, while horizontal comes out very nearly right.
Sixteen times gain vertically drives the cursor into the top edge and holds it there,
which is exactly what you described. It now reads the descriptor the mouse itself
publishes instead of assuming, so it should work on yours and on models neither of us
has. I could not test it against your mouse — if it still misbehaves, one diagnostic log
will tell me everything, because it now records the descriptor and what it made of it.

On speed: there is no sensitivity setting yet. I think most of "uncontrollably fast" was
the misreading, so try it decoded properly first — if it is still too quick, say so and I
will add one. I would rather not add a knob to compensate for a bug.

**60m showing as "0m".** The Tab5 reads band names out of your radio's own band table,
and it is getting "0" rather than "60" from yours. Mine reports it correctly so I cannot
reproduce it, but the Tab5 logs the name it got for every band slot — could you send me a
diagnostic log (Files → "Diagnostic download") when convenient? This is the one item on
your list I have not solved.

**CW decode.** Not in a release yet — what you read is the roadmap. The QMX+ decodes
internally and mirroring that over CAT is the plan.

**"Release radio".** You were right, and it is reworded. The old wording described what
the software does to the radio; it now says what you are trying to do. The button reads
**"Let me use the QMX menus"**, and while it is handed over, **"Done - Tab5 takes over
again"**. The pause and play icons are gone too — a gear now, pointing at where you are
headed, since a tape-deck symbol was never the right metaphor.

On placement: while the radio is handed over there is already a bar across the top you
can tap to take it back, so the way out is at your fingertips — it is the way in that is
buried in Settings. I have not moved it yet, because the bottom bar is already carrying
battery, SD, version, clock, Bluetooth, WiFi, network name and IP, and I would rather
find it a proper home than wedge it in. Tell me if you would still prefer it there and I
will make room.

**The web interface.** Your question about black level and contrast turned out to be the
most productive thing in your list. The answer when you asked was no, they did not affect
the browser at all — it drew its own picture with its own noise floor and its own colours.
Chasing that found four separate faults, and the last one was on the radio side: the Tab5
re-seeds its noise floor about seventeen times a second, so the per-bin adaptive floor
has never actually run on any version. What it draws is each bin measured against the
average across the band, refreshed constantly, which is why it settles instantly and
never lets a signal fade. The browser now does the same and the two look alike. One
consequence worth knowing: the **"Adaptive floor" setting does nothing, and never has**.
I have left the behaviour alone deliberately — the alternative is what it was designed
for, where a steady carrier sinks out of sight over about a minute, and I do not think
anyone wants that on a panadapter. But a setting that cannot change anything is worse
than a missing one, so I have taken it out of the browser's settings page.

Thank you for pushing on the tuning one after Roy could not reproduce it. A second report
that disagrees with the first is usually the most useful thing in a bug hunt, and this
one was.

---

## Post 2 — to everyone: check your SWR protection

If you have ever set **SWR protection** from the browser's Settings page, it was not
saved. The field accepted your value, reported success, and the device discarded it —
reload the page and it read back whatever it was before, which is easy to miss.

Please check it on the Tab5 itself: Settings → "SWR protection (transmit)". If it says
Off and you thought you had set it, that is this bug and not you.

Setting it on the Tab5 has always worked. Only the browser path was affected, on every
v1.7.x build. Fixed in **v1.8.0**, where the browser will also offer the same four
thresholds the Tab5 does instead of asking you to type a number.

My apologies — a control whose whole job is to stop a transmission into a bad antenna is
the last one that should fail quietly.

---

## Post 3 (OPTIONAL) — to John Dusek, on running without a battery

John — Don and Randy have it right, and to confirm from the firmware side: nothing in the
panadapter needs the battery. It runs perfectly on USB-C alone, and the Tab5 powers up as
soon as you plug it in.

The only thing you lose is the battery readout: with no pack detected the bottom bar
shows a red battery symbol with a line through it and no percentage, which is telling you
the truth rather than complaining. The battery-care setting under Settings also has
nothing to do — it limits how full the pack is charged.

If you do run it on the bench permanently, USB-C is the option I would pick — it is the
one that manages charging properly if you ever add a pack later.
