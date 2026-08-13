# Replies to send — 2026-08-13

Everything from this session's reports, in one place. Each section is ready to
paste. Nothing has been pushed or posted.

**Status key:** ✅ fixed and verified · 🔧 fixed, needs a field check ·
❓ needs information from the user · 💭 no action yet

---

## 1. Stan KC7XE — dual-VFO display ✅ (documented, no code change)

> Stan, thank you — that settles it, and it matches what I was seeing from the
> other side. My read-back said split was off and VFO B equalled VFO A, and the
> LCD kept showing both anyway. Knowing nothing repaints the second row explains
> exactly that, and the older thread you linked confirms it is not new.
>
> I have decided against a workaround in the panadapter, and I want to say why in
> case you disagree. A brief CW-mode switch only clears the row if CW decode is
> enabled, which I cannot know from my side. And I tried a mode bounce once before
> for a different problem — forcing the SSB filter to reload — and it worked but
> flickered the mode indicator on every change, so I took it out. Trading a
> cosmetic radio artifact for a visible mode flicker on every stand-down does not
> feel like a good deal.
>
> Instead the manual now says plainly that a second frequency left on the LCD is a
> display artifact, that VFO B equals VFO A and split is off, and that both are
> confirmed by read-back. It also warns people off `MU;` — it clears the display
> but silently drops I/Q mode, and the spectrum goes flat while the radio carries
> on streaming audio as if nothing happened. That one cost me an evening.
>
> If Hans ever repaints that row on a CAT-driven VFO change, I will happily delete
> the note.

---

## 2. Don WB0LQW — two separate things

### 2a. The clock: GPS-less QMX overwriting good UTC ✅ FIXED and verified

> Don, you were not missing anything — this was a real bug and you described the
> mechanism correctly. Fixed.
>
> What was happening: the panadapter only ever protected the clock from the radio
> when SNTP was fresh. Offline SNTP is never fresh, so the protection could not
> possibly help the one case it mattered for — yours. On the first poll after you
> switched the radio on, the QMX's 00:00 replaced your accurate RTC time.
>
> A QMX without GPS is now treated as what it is: not a time reference. Its RTC
> free-runs and restarts at 00:00 after any power-off, while the Tab5's supercap
> RTC holds seconds-accurate UTC for 30–40 hours. So the radio is refused whenever
> the Tab5 holds a clock it trusts, and the Tab5 pushes its time **to** the radio
> instead — which is what you suggested. It only pushes when the radio is more than
> 3 seconds out, so it is not chattering over CAT every five minutes.
>
> I reproduced your exact situation on the bench and this is the log from the fix:
>
> ```
> QMX TM; 00:00:08 ignored - radio has no GPS and our clock is trusted (SNTP, 27992 s apart)
> Tab5->QMX time push: 07:46:40 UTC
> ```
>
> Radio at its power-on 00:00, Tab5 clock good, 7.8 hours apart. Before the fix
> that 00:00 was applied. Now it is refused and the radio gets set.
>
> The manual's offline section now has the missing step you pointed out — step 4
> says to turn the QMX on whenever you like and that you do not have to do
> anything about its clock, with a note explaining what changed. It also says what
> happens if you have no good time at all: then a QMX reading *is* used, because
> something beats nothing, and failing that you can set the clock by hand under
> FT8 → Filter → Sync Time, which takes seconds as well as hours and minutes.

### 2b. The CQ presets ❓ I need one diagnostic log

> On the CQ messages — thank you for that test, the dummy load and the second
> receiver is exactly the setup that makes this findable.
>
> I have run your three presets through the message encoder on the bench, and
> **all of them encode correctly**, including `CQ POTA WB0LQW` with and without
> your grid:
>
> ```
> CQ WB0LQW DN70        -> 'CQ WB0LQW DN70'
> CQ POTA WB0LQW        -> 'CQ POTA WB0LQW'
> CQ QRP WB0LQW         -> 'CQ QRP WB0LQW'
> CQ POTA WB0LQW DN70   -> 'CQ POTA WB0LQW DN70'
> CQ QRP WB0LQW DN70    -> 'CQ QRP WB0LQW DN70'
> ```
>
> Each one round-trips back byte-for-byte, so the message format is not the
> problem and `CQ POTA` / `CQ QRP` are perfectly legal. The preset field is also
> wide enough that none of them are being truncated.
>
> I did find and fix one real bug from your report: **the Operator Identity window
> was appearing for any message that would not build**, whatever the reason. Your
> callsign and grid were fine — the panadapter just pointed you at them because
> its error handling was lazy. That is why you went looking in the wrong place. It
> now tells you what actually went wrong.
>
> But I have not found the cause of your main symptom, and I would rather say so
> than guess. After the message is built, every preset takes an identical path to
> the air — same tones, same timing, same CAT commands — so there is nothing left
> in my code that would modulate one correctly and another not at all.
>
> Could you reproduce it once more and send me a diagnostic log? Set up as you did
> before, select Msg 2, transmit, then on the Tab5: **Files → "Diagnostic
> download ↓"** and send me the file. There is a line in it that reads
> `built text CQ: '...'` which tells me immediately whether the fault is before or
> after the message is built, and that decides where I look next.

---

## 3. Roy KI0ER — same CQ question ❓

> Roy, thank you for confirming Don's symptom — two reports of the same thing is
> much more useful than one.
>
> Short version of what I have found so far: the message encoding is fine. I ran
> `CQ POTA <call>` and `CQ QRP <call>` through the encoder with and without a grid
> and all of them encode and decode correctly, so the preset format is not the
> problem. I also fixed a bad error message that was blaming the operator's
> callsign whenever a message would not build, which is what sent Don looking in
> the wrong place.
>
> I have not found the real cause yet. Could you send me a diagnostic log of it
> happening — **Files → "Diagnostic download ↓"** after reproducing it? Having
> yours as well as Don's tells me whether this is something about a particular
> unit or something in the code, which is the first thing I need to know.

### 3b. Roy on RIT — thank you, and one request built 🔧

> Roy, thank you for the RIT explanation — that is a better answer than the one I
> was about to give. I had been about to tell Samuel that RIT is useful in FT8 as
> well, and you are right that it is not; the reason it is not shown there is
> precisely that there is nothing for it to do. Good to have that stated by someone
> who has actually used it in anger, and in more modes than I have.
>
> Your round-robin case is one I had not thought about at all, and the request that
> comes with it is a good one: **turn RIT off and back on again at the same offset,
> with a long press.** That is how RIT works on a rig with a dedicated button —
> clearing the offset and switching it off are two different actions, and the
> panadapter only really offered the first. That is now built.
>
> Long press parks the offset and stands RIT down; long press again puts it back
> unchanged. Short press is as it was. While an offset is parked the button reads
> `RIT (+200)` in brackets and in the dim colour — the radio really is back on
> frequency so it must not look engaged, but a plain `RIT` would give you nothing
> to say there was something to bring back.
>
> One decision I made that you may want to argue with: the parked offset is
> discarded when you retune. It belongs to the station that was off frequency, and
> restoring it after a band change or a spot click would put your receiver
> somewhere you never asked for, from a number you could no longer see. If you
> would rather it survived a retune, say so — it is a one-line change.
>
> On XIT: agreed, and that is why the panadapter's version of it is deliberately
> CW-only. It does the same job by holding VFO B at an offset and running split,
> since the QMX has no XIT of its own, and it stands itself down the moment you
> leave CW. An offset transmit in SSB or a digital mode is a mistake rather than a
> courtesy.

---

## 4. Samuel W7STF — two of four built 🔧, two need your view 💭

> Samuel, taking your points in order.
>
> **How many users:** I do not have a real number. Downloads are not people, and
> most people who are getting on fine never write. The list I actually hear from
> is maybe fifteen or twenty, and honestly that is what shapes the releases — your
> reports, Roy's, Don's, Michael's, Dirk's. So the thanks does go both ways.
>
> **The mouse:** unchanged in v1.8.1, and I will redo it properly. I found the
> main fault — the mouse's report descriptor was being read short, so the layout
> was parsed wrongly — but my fix broke the connection sequence and I took it out
> rather than ship it. When I have the rewrite I will ask you for a diagnostic log
> first, because your symptom does not match that fault and I think there is a
> second one on your mouse specifically.
>
> **The hour-long USB disconnect:** please do bring things like that up sooner. I
> chased it from the Tab5 end with six different approaches and none of them
> helped — once the radio stops answering, only a power cycle of the QMX clears
> it. That one is with QRP Labs now.
>
> **RIT in modes other than CW:** Roy has answered this better than I could, and
> I would go with his explanation over mine — it is not a CW-only control, it is
> for compensating another station being off frequency, in whatever mode. I will
> add only that the panadapter behaves the way he described: the RIT button is
> hidden in FT8/FT4, because there it has nothing useful to do. If you do not want
> it in CW and SSB either, the drawer switch in v1.8.1 gives you the corner back —
> and an engaged offset still shows itself regardless, so RIT can never be on
> without something on screen saying so.
>
> **Showing the offsets on the spectrum or waterfall: done.** With RIT engaged the
> offset now prints in magenta beside its own marker over the waterfall — `+250 Hz`
> next to the line that shows where you are actually listening. That is a better
> home for it than the corner, as you said: the marker already says *where*, so the
> number saying *how far* belongs next to it.
>
> **The band strip when out of band: done, and you were right.** It no longer
> vanishes. It fills with one flat block reading "Out of band", and goes back to
> the CW/Digi/Phone colours the moment you are inside a band again. The marker and
> the little window frame stay hidden while you are out — there is no band for them
> to be positioned against, and drawing them anyway would be inventing a scale.
> The strip staying put also keeps the coarse-tune drag where your thumb expects
> it, which I think was half your point.
>
> **Arrow buttons scaled by zoom: I want to build this but I do not know where to
> put them.** Your sizing is right — a quarter of the visible span per press and a
> half for a double arrow makes the step mean the same thing to the eye at every
> zoom, which a fixed step in hertz never does. The problem is space. The top bar
> is genuinely full: Band, Mode, BW, then the frequency, then the S-meter, then
> Zoom, and the only gap is about 70 pixels. I am also reluctant to redesign that
> bar because the same one serves the panadapter, FT8 and the CW page.
>
> So: would arrows somewhere other than beside the frequency still solve your
> problem? Your actual complaint was having to click the far left and right of the
> spectrum to step the VFO, and buttons anywhere reachable would fix that. Options
> I can see are a small cluster at one end of the frequency-axis row under the
> spectrum, or on the band-plan strip at the bottom next to where your thumb
> already goes. Tell me which you would actually use and I will build it there.
>
> **A VFO A/B button: I need to push back on this one, at least for now.** The
> panadapter already uses split for the CW transmit offset — it holds VFO B at your
> offset and re-asserts it every 30 seconds, and stands it down when you leave CW.
> A VFO A/B control would be a second thing steering the same two VFOs, and the
> first symptom would be the two fighting each other mid-QSO.
>
> It would also land straight on top of the QMX display bug in this same thread:
> the radio's LCD does not repaint the second row when the VFO mode changes over
> CAT, so a working A/B button would frequently *look* broken through no fault of
> its own.
>
> If what you want is mostly to *see* which VFO is active, that I can do safely as
> an indicator. A control that switches them needs the split interaction thought
> through first, and I would rather do that properly than ship something that
> misbehaves while you are transmitting. What would you actually use it for?
>
> **Top-bar labels and matching the browser:** agreed, and that is the direction
> I am going. The same bar has to serve the panadapter, FT8 and the CW page, so it
> changes slowly.

### 4b. Samuel's follow-up on RIT in a net 🔧

> Samuel, one correction and then you are right anyway.
>
> RIT cannot put anyone else off frequency. It shifts only what *you* hear — your
> transmit does not move at all. So in a three-way or a net, the risk is not that
> you drift away from the group; it is that while you are compensating for the one
> station who is off, everybody else in the net now sounds off-pitch to *you*.
>
> Which is exactly the argument for a quick off, so your conclusion is right even
> though the mechanism is the other way round. And it is now built, from Roy's
> request: **long-press the RIT button and the offset is parked and switched off;
> long-press again and it comes back unchanged.** Short press still clears it
> outright. So in your net you can drop the offset when the turn passes back to the
> stations who are on frequency, and pick it up again when it is that one operator's
> turn, without re-dialling anything.
>
> Your other worry — nudging it by accident and forgetting — is the one I care about
> most, and the panadapter is built so it cannot happen quietly. **An engaged RIT
> always shows itself**, even if you have switched the button off in the settings; a
> radio listening 250 Hz away with nothing on screen saying so is a bug, not a tidy
> screen. As of your message that now covers a *parked* offset too: it shows as
> `RIT (+250)` in brackets, so "there is an offset waiting to come back" is never
> invisible either. Thank you for that — you found it by thinking out loud.
>
> On the transmit side you are right that it is a different matter, because that one
> genuinely does move you relative to everyone else. The QMX has no XIT, so the
> panadapter's CW transmit offset does the job with split — and it stands itself
> down the moment you leave CW, precisely so it cannot leave you transmitting off
> frequency in SSB or AM.

---

## 5. For the beta testers generally 🔧 — new: spur suppression (off by default)

> There is something new on the next build that some of you may find interesting,
> and it needs testing by people other than me.
>
> If you have ever seen evenly-spaced signals on the waterfall that are always
> there, do not move when you tune, and are still there with the antenna
> disconnected — those are not signals. They come from the radio's own frequency
> synthesizer, and how strong they are depends on exactly where you are tuned. On
> my unit at 14.074 — the FT8 calling frequency, of all places — the strongest one
> sits nearly 40 dB above the noise floor, and the noise floor itself is a few dB
> worse there.
>
> The panadapter can now find and remove them. It works by nudging the dial 25 Hz
> for about two seconds: a real signal stays where it is, while one of these
> artifacts jumps a long way, because they move sixteen to fifty times faster than
> the dial does. Whatever it finds is remembered per frequency, so coming back to
> a frequency you have used costs nothing.
>
> It is **off by default** and I would like it to stay opt-in until a few of you
> have lived with it. **Settings → Waterfall → Spur suppression:**
>
> - **Off** — exactly as before, nothing is touched.
> - **Subtract spur power** — takes away the measured artifact. It cannot hide a
>   real signal, but the artifact stays faintly visible.
> - **Erase spur bins** — the artifact disappears completely. The cost is that a
>   real signal sitting exactly on one is hidden while you sit still. Nudging the
>   dial moves the blind spot right off it, because these things shift so much
>   faster than the dial.
>
> Whichever you choose, the line just under the frequency labels turns teal wherever
> something is being removed, so you can always see what the firmware is touching
> rather than having to trust it.
>
> What I would like to know: does it find your spurs, does the two-second nudge
> bother you in normal operating, and does Erase ever hide something you wanted?

---

## Notes for me (not for sending)

- Commits: `992fd54` spur suppression · `c7fd65b` harness + marker ·
  `72d58c0` CQ error reporting + encode harness · `8a4f3f4` QMX time priority.
  **Nothing pushed.**
- The mkdocs note for Stan's item is in `docs/mkdocs/guide/settings.md`. It reaches
  the website and the on-device manual on the next docs build; `manual.bin` is not
  regenerated until then.
- Don's manual gap: **done**. `docs/mkdocs/guide/time-sync.md`, which the PDF
  builder injects (line 102 of `build_userguide_pdf.ps1`) as well as the website
  and the on-device manual — one edit, all three trees. `check_docs.py` clean.
- Samuel: **RIT-offset readout and the out-of-band band strip are built and
  screen-verified** (`/ss.bmp`: strip uniform 49,56,66 with "Out of band" centred;
  label "+250 Hz" at x≈653 beside the marker at x≈647). **Arrow buttons and VFO
  A/B are not built** — both need his answer first. Arrows have nowhere to live in
  a full top bar the operator has said he is keeping; VFO A/B would fight
  `cw_split_maintain()` and would inherit the QMX's own LCD repaint bug.
- Samuel's follow-up exposed a gap in the long-press: with the pill hidden, an
  offset engaged from the browser and then parked, `rit_now` returns to 0 and the
  pill hid itself again - leaving the parked offset invisible AND unreachable,
  since restoring it needs the pill to be there. A parked offset now keeps the
  pill visible, same rule as an engaged one.
- Roy's RIT long-press: **built, gesture UNVERIFIED.** I can engage RIT over the
  API but cannot perform a touch hold, so the hold itself needs a finger. Test:
  engage RIT, long-press the pill (offset clears, label reads `RIT (+200)`),
  long-press again (offset returns), then retune (label back to plain `RIT`,
  nothing parked). RIT is currently set to +200 on the bench unit ready for it.
- I nearly told Samuel that RIT is useful in FT8. It is not, our own code says so
  in a comment, and Roy would have corrected me in public. Check the code before
  explaining behaviour to operators who use it daily.
- Still open from earlier: SDIO recoveries clustering (1–2/boot where they should
  be 0) — needs a feed-by-feed A/B soak, not a guess.
