# v1.8.2 announcement — draft for groups.io

Plain text, ready to paste. Review before posting.

---

**QMX Panadapter v1.8.2 is out.**

Everything in this one came from your reports this week. Thank you.

**Your radio makes its own spurs, and they can now be taken off the display.**

If you have ever seen evenly spaced signals that sit in the same place, never move
when you tune, and are still there with the antenna unplugged — those are not
signals. They come from the QMX's own synthesizer, and how strong they are depends
on exactly where you are tuned. On my unit at 14.074 MHz, the FT8 calling frequency
of all places, the strongest sat nearly 40 dB above the noise floor, and the noise
floor itself was a few dB worse there than 6 kHz away.

The panadapter can now find them. It nudges the dial 25 Hz for about two seconds: a
real signal stays where it is, while these move sixteen to fifty times further,
because their position follows the synthesizer rather than the band. What it learns
is remembered per frequency, so returning to a frequency you have already used
costs nothing.

Settings -> Waterfall -> Spur suppression. **Off by default**, and I would like it
to stay opt-in until a few of you have lived with it.

- Subtract spur power — removes the measured artifact. This can never hide a real
  signal. The artifact stays faintly visible.
- Erase spur bins — the artifact disappears completely. The cost is that a real
  signal sitting exactly on one is hidden while the dial sits still. Nudging the
  dial slides the blind spot off it.

Wherever something is being removed, the thin line under the frequency labels turns
teal, so you can always see what is being touched rather than having to trust it.

What I would like to know: does it find yours, does the two-second nudge bother you
in normal operating, and does Erase ever hide something you wanted?

**A QMX without GPS no longer overwrites an accurate clock.** Set the Tab5's RTC at
home, arrive at a POTA site, switch the radio on — and your accurate UTC was
replaced by the radio's power-on 00:00, after which FT8 stopped decoding. The clock
was only ever protected while network time was fresh, and offline it never is. The
Tab5's own RTC now wins, and it sets the radio's clock instead. Thanks Don WB0LQW,
who described the mechanism exactly.

**RIT can be parked instead of cleared.** Long-press the RIT button and the offset
is remembered while RIT switches off; long-press again and it comes back unchanged.
For a net or a round robin where one station is off frequency, the offset comes and
goes as the turn passes, without re-dialling it each time. The button reads
`RIT (+250)` while an offset is parked. Thanks Roy KI0ER.

**The RIT offset is shown on the waterfall** now, beside its own marker, so you can
read how far off you are listening without looking at the corner. And **the band
strip no longer vanishes when you are out of band** — it reads "Out of band" in one
flat colour and comes back to normal as soon as you are inside a band, so the row is
never just empty and the coarse-tune drag stays where your thumb expects it. Both
asked for by Samuel W7STF.

**Two things that were making the panadapter look guilty of faults it did not
cause.** Calling CQ with a message that would not build used to pop the Operator
Identity window whatever the actual reason, so an operator with a perfectly good
callsign was sent to check it — it now shows the real error. And after certain
restarts the QMX stops answering on USB until it is power-cycled; the recovery meant
for a stuck USB port was firing at that and cutting the port's 5 V, which switches
the radio off in front of you and does not help. It now recognises the difference
and leaves the radio alone.

**Still open, and I would like a log if it happens to you:** CQ presets beyond a
plain "CQ <call> <grid>" not being decoded at the far end, reported by Don WB0LQW
and Roy KI0ER. I have run those presets through the message encoder on the bench and
they encode correctly, so the format is not the problem and I have not found the
cause yet. If you see it, please reproduce it and send me Files -> "Diagnostic
download".

Full detail: https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md
Download: https://github.com/SteffenLav/qmx-panadapter/releases

73 Steffen OZ1LAV

---

## Not for posting — checklist state at the time of writing

- Tag `v1.8.2` at `99337e3`, local only. **Not pushed.**
- Binary verified: magic `0xabcd5432`, version exactly `v1.8.2`, and the port-only
  replug fix confirmed present in the flashed image.
- `manual.bin` fresh: `21 entries, 280 KB` (19 nav pages + toc.json + A-Z index).
- `latest.json` = v1.8.2, generated after the tag.
- Flasher ZIP rebuilt and **verified end-to-end from the extracted ZIP** — component
  addresses 0x2000/0x8000/0x10000, all three hashes verified, device booted to
  v1.8.2 and the QMX connected (`1_04_004QMX`, audio 48k pairs/s).
- `check_docs.py`: 0 errors, 2 pre-existing warnings.
- Both esp_hosted patches re-applied after the clean build wiped them.
- **site/ must be FTP'd to tab5.lav.dk** — there is no auto-deploy.
- Unexercised: the replug gate and the port-only retry (both only fire when the
  radio wedges), and the parked-RIT-with-pill-hidden case.
- Not done by me: FT8/FT4 mode switching check from the Phase 5.5 list, which needs
  the screen.
