# Replies to send — 2026-08-13

Everything from this session's reports, ready to paste. Nothing pushed or posted.

**Status key:** ✅ fixed and verified · 🔧 fixed, needs a field check ·
❓ needs information · 💭 no action yet

---

## 1. Stan KC7XE — dual-VFO display ✅

> Stan, thank you. That settles it.
>
> **What I saw from my side:** Read-back said split off and VFO B equal to VFO A.
> The LCD showed both anyway. A second row that never repaints explains that
> exactly.
>
> **No workaround in the panadapter:** A brief CW-mode switch only clears the row
> when CW decode is enabled. I cannot know that from my side. I also tried a mode
> bounce once before to force the SSB filter to reload. It worked, but it flickered
> the mode indicator on every change, so I removed it. A cosmetic radio artifact is
> the better of the two.
>
> **What I did instead:** The manual now states that a second frequency on the LCD
> is a display artifact, that VFO B equals VFO A, and that split is off. Both are
> confirmed by read-back.
>
> **A warning about `MU;`:** It clears the display and silently drops I/Q mode. The
> spectrum goes flat while the radio keeps streaming audio. The manual says so now.
>
> If Hans ever repaints that row on a CAT-driven VFO change, I will delete the note.

---

## 2. Don WB0LQW — two items

### 2a. Clock: GPS-less QMX overwriting good UTC ✅

> Don, you were not missing anything. Real bug, and you described the mechanism
> correctly. Fixed.
>
> **What was wrong:** The panadapter only protected the clock while SNTP was fresh.
> Offline SNTP is never fresh. So the protection could not help the one case that
> needed it. On the first poll after you switched the radio on, its 00:00 replaced
> your RTC time.
>
> **What it does now:** A QMX without GPS is not treated as a time reference. Its
> RTC free-runs and restarts at 00:00 after a power-off. The Tab5's supercap RTC
> holds seconds-accurate UTC for 30–40 hours. So the radio is refused while the
> Tab5 holds a clock it trusts, and the Tab5 sets the radio instead. That is what
> you suggested. It only writes when the radio is more than 3 seconds out.
>
> **Bench proof:** I reproduced your situation exactly.
>
> ```
> QMX TM; 00:00:08 ignored - radio has no GPS and our clock is trusted (SNTP, 27992 s apart)
> Tab5->QMX time push: 07:46:40 UTC
> ```
>
> Radio at power-on 00:00. Tab5 clock good. 7.8 hours apart. Before the fix that
> 00:00 was applied.
>
> **Manual:** Step 4 of the offline section now says to turn the QMX on whenever you
> like and that its clock needs nothing from you.
>
> **If you have no good time at all:** A QMX reading is used, because something
> beats nothing. Failing that, set it by hand under FT8 → Filter → Sync Time, which
> takes seconds as well as hours and minutes.

### 2b. CQ presets ❓

> **Your test setup:** The dummy load and a second receiver is what makes this
> findable. Thank you.
>
> **The encoder is not the problem.** I ran your three presets through it on the
> bench. All five variants encode and round-trip byte-for-byte.
>
> ```
> CQ WB0LQW DN70        -> 'CQ WB0LQW DN70'
> CQ POTA WB0LQW        -> 'CQ POTA WB0LQW'
> CQ QRP WB0LQW         -> 'CQ QRP WB0LQW'
> CQ POTA WB0LQW DN70   -> 'CQ POTA WB0LQW DN70'
> CQ QRP WB0LQW DN70    -> 'CQ QRP WB0LQW DN70'
> ```
>
> `CQ POTA` and `CQ QRP` are legal. The preset field is wide enough that none of
> them truncate.
>
> **One real bug found and fixed:** The Operator Identity window appeared for any
> message that would not build, whatever the reason. Your callsign and grid were
> fine. The panadapter pointed you at them because its error handling was lazy. It
> now reports the actual error.
>
> **What I have not found:** The cause of your main symptom. After the message is
> built, every preset takes the same path to the air. Same tones, same timing, same
> CAT commands. Nothing in my code would modulate one correctly and another not at
> all.
>
> **What I need:** One diagnostic log. Set up as before, select Msg 2, transmit,
> then **Files → "Diagnostic download ↓"** and send me the file. It contains a line
> reading `built text CQ: '...'`. That tells me whether the fault is before or
> after the message is built, and decides where I look next.

---

## 3. Roy KI0ER — two items

### 3a. CQ presets ❓

> **Two reports beat one.** Thank you for confirming Don's symptom.
>
> **Where it stands:** The encoding is fine. `CQ POTA <call>` and `CQ QRP <call>`
> encode and decode correctly with and without a grid. I also fixed an error
> message that blamed the operator's callsign whenever a message would not build,
> which is what sent Don looking in the wrong place.
>
> **Not found yet.** Could you send a diagnostic log of it happening?
> **Files → "Diagnostic download ↓"** after reproducing it. Yours alongside Don's
> tells me whether this is one unit or the code. That is the first thing I need.

### 3b. RIT ✅

> **Your explanation is better than mine was.** I had been about to tell Samuel that
> RIT is useful in FT8. It is not. The reason it is hidden there is that it has
> nothing to do.
>
> **Your request is built:** Long press parks the offset and switches RIT off. Long
> press again restores it unchanged. Short press still clears it outright. Tested
> here. Parks and restores cleanly.
>
> **While parked:** The button reads `RIT (+200)` in brackets and in the dim colour.
> The radio is back on frequency so it must not look engaged. A plain `RIT` would
> leave nothing to say there was an offset waiting.
>
> **One decision you may want to argue with:** The parked offset is discarded when
> you retune. It belongs to the station that was off frequency. Restoring it after a
> band change would move your receiver from a number you can no longer see. Say the
> word and it survives retuning instead. One line.
>
> **On XIT:** Agreed, and that is why the panadapter's version is CW-only. It holds
> VFO B at an offset and runs split, since the QMX has no XIT. It stands down the
> moment you leave CW.

---

## 4. Samuel W7STF — two built 🔧, two need your view 💭

> **Offsets on the spectrum: done.** With RIT engaged the offset prints beside its
> own marker over the waterfall, as `+250 Hz`.
>
> **Band strip out of band: done, and you were right.** It no longer vanishes. One
> flat block reading "Out of band", back to normal colours as soon as you are in a
> band. The coarse-tune drag stays where your thumb expects it.
>
> **RIT outside CW:** Roy answered it. The button is hidden in FT8 and FT4 because
> there it has nothing to do.
>
> **The mouse:** Unchanged in v1.8.1. I found the main fault but my fix broke the
> connection sequence, so I pulled it. I will ask you for a diagnostic log when the
> rewrite is ready. Your symptom does not match that fault, so I suspect a second
> one on your mouse.
>
> **The hour-long USB disconnect:** Please raise things like that sooner. Six
> approaches from the Tab5 end, none helped. Only a QMX power cycle clears it. It is
> with QRP Labs.
>
> **How many users:** No real number. Fifteen or twenty write to me, and that is
> what shapes the releases.
>
> **Arrow buttons: yes, and your sizing is right.** A quarter of the visible span,
> half for a double arrow. I cannot put them beside the frequency though — the top
> bar is full and that same bar serves three screens. Pick one and I will build it
> there:
>
> **(a)** the frequency-axis row under the spectrum, or **(b)** the band-plan strip
> at the bottom.
>
> **VFO A/B: not yet.** The panadapter already drives both VFOs for the CW transmit
> offset, so a second control would fight it mid-QSO. It would also inherit the LCD
> repaint bug from Stan's thread and look broken when it was not. I can show which
> VFO is active safely enough. Switching them needs more thought.

### 4b. Samuel on RIT in a net 🔧

> **RIT cannot put anyone else off frequency.** It shifts only what you hear. Your
> transmit does not move. The risk is the reverse: while you compensate for the one
> station who is off, everybody else sounds off-pitch to you.
>
> **Your conclusion still holds.** That is the argument for a quick off, and it is
> built. Long press parks the offset, long press again restores it.
>
> **Nudging it by accident:** An engaged RIT always shows itself, even with the
> button switched off in settings. Your message made me extend that to a parked
> offset, which now reads `RIT (+250)`.

---

## 5. Beta testers — spur suppression, off by default 🔧

> **Something new, and it needs testing by people other than me.**
>
> **The symptom:** Evenly-spaced signals on the waterfall that are always there, do
> not move when you tune, and remain with the antenna disconnected. Those are not
> signals. They come from the radio's own synthesizer, and their strength depends on
> where you are tuned. On my unit at 14.074, which is the FT8 calling frequency, the
> strongest sits nearly 40 dB above the noise floor. The floor itself is a few dB
> worse there.
>
> **How it finds them:** It nudges the dial 25 Hz for about two seconds. A real
> signal stays put. These artifacts jump a long way, because they move sixteen to
> fifty times faster than the dial. Results are remembered per frequency, so
> returning to a frequency you have used costs nothing.
>
> **It is off by default** and I want it opt-in until some of you have lived with it.
> **Settings → Waterfall → Spur suppression:**
>
> - **Off** — nothing is touched.
> - **Subtract spur power** — removes the measured artifact. It cannot hide a real
>   signal. The artifact stays faintly visible.
> - **Erase spur bins** — the artifact disappears. A real signal sitting exactly on
>   one is hidden while you sit still. Nudging the dial moves the blind spot off it.
>
> **What is being touched:** The line under the frequency labels turns teal wherever
> something is removed. You can always see it rather than having to trust it.
>
> **What I want to know:** Does it find your spurs? Does the two-second nudge bother
> you in normal operating? Does Erase ever hide something you wanted?

---

## Notes for me (not for sending)

- Commits: `992fd54` spur suppression · `c7fd65b` harness + marker ·
  `72d58c0` CQ error reporting + encode harness · `8a4f3f4` QMX time priority ·
  `12be1a2` RIT long-press + offline manual · `0c8a36e` OOB strip + RIT readout ·
  `ac18351` replug gate · `e23ac80` parked-RIT visibility. **Nothing pushed.**
- Stan's note is in `docs/mkdocs/guide/settings.md`. It reaches the website and the
  on-device manual on the next docs build. `manual.bin` is not regenerated until then.
- Don's manual gap is in `docs/mkdocs/guide/time-sync.md`, which the PDF builder
  injects (line 102 of `build_userguide_pdf.ps1`). One edit covers all three trees.
  `check_docs.py` clean.
- Samuel: RIT readout and OOB strip are screen-verified via `/ss.bmp` (strip uniform
  49,56,66 with "Out of band" centred; label "+250 Hz" at x≈653 beside the marker at
  x≈647). Arrow buttons and VFO A/B need his answer first.
- RIT long-press: hardware-verified by the operator. Still untested by anyone: the
  parked-offset-keeps-the-pill-visible case, which needs the pill switched OFF in
  settings first.
- I nearly told Samuel that RIT is useful in FT8. Our own code says otherwise in a
  comment, and Roy would have corrected me in public. Check the code before
  explaining behaviour to operators who use it daily.
- The replug gate has NOT been exercised. It only fires when the radio wedges. The
  tell will be `RADIO-side wedge, not a stuck port. Not replugging` with the radio
  staying on.
- Still open: SDIO recoveries clustering (1–2/boot where they should be 0). Needs a
  feed-by-feed A/B soak, not a guess.
