**A same-day fix release. v1.8.5 shipped with the browser interface completely dead, and that is the headline.**

**The web UI works again (#183 — Randy N4OPI, Michael KZ4LY).** One unterminated string literal in the page stopped the *entire* script running, so the browser drew its controls and then did nothing at all: no spectrum, no waterfall, no working buttons, "disconnected" in the corner, in both Chrome and Firefox. Anyone who used the browser had nothing.

It was not a typo. The literal was written by a script whose shell collapsed a `
` into a real newline before it was ever saved — the same collapsing that produced a NUL byte in one C file and a stray control byte in another on the same evening. The compiler caught both of those instantly; nothing caught this one, because the firmware build had no reason to parse HTML. **It does now**: the build extracts the page's script and refuses to compile if it does not parse, and that check was tested by re-breaking the literal exactly the way it shipped.

**A crash that was blamed on the radio (#182).** An overnight soak of v1.8.5 aborted at 1 h 57 m of a completely healthy FT8 session on `usb_dwc_hal.c`'s assertion that "an error should have halted the channel" — an assumption ESP32-P4 silicon does not honour. The reboot is the cheap part: an abort is a warm reset with the QMX attached, which is the one condition that leaves the radio unable to re-enumerate, so it then stayed dead for the remaining 5 h 26 m. The morning's report was "the QMX wedged during the night". The QMX was fine. This is now **standing patch #7**, and it reports the error instead of aborting — the recovery path was already written directly beneath the assertion.

**CW frequency display and tap-to-tune (#165 — Roy KI0ER).** A signal transmitted on 7.060.000 showed at 7.060.040, and tapping it tuned him 40 Hz off, so the far station heard him shifted. Two separate faults added up to that figure:

- The per-unit CW trim defaulted to **−60 Hz**. It arrived with the commit that first read the CW offset from the radio over CAT, so it was calibrated *before* that reading existed and never revisited. Worth a flat +60 Hz.
- The display offset was **rounded to a whole FFT bin**. A bin is 46.88 Hz, so that alone can misplace everything by up to 23 Hz — and by an amount that changes with the CW offset you choose, which is why the errors looked non-linear in the value set.

The trim now defaults to 0 (the slider stays, for real per-unit trimming) and the rounding remainder is compensated where it matters: a tap converts screen position to frequency correctly, and the dial marker is drawn on the signal rather than beside it. The arithmetic reproduces **both** of Roy's measurements — 650 Hz predicts +41, 700 Hz predicts +44, against his reported +40 — and both go to zero.

✅ **CONFIRMED ON A REAL SIGNAL** (Roy KI0ER, 2026-08-18, v1.8.6 + QMX 1.04.007): *"an incoming CW signal's peak on the waterfall aligns properly with the actual frequency now. If it is off a tad, it appears it's within 5 Hz."* Tested at his 650 Hz CW centre; other centre/offset/tone settings not re-tested. This shipped verified only by arithmetic matching his earlier numbers - the measurement has now caught up with it.

**Radio menus, all from Michael KZ4LY using it:**

- **You can see what you are typing past message 9.** The keyboard covers the lower half of the screen, so tall menus were edited blind. He suggested making the keyboard transparent; the screen now scrolls instead so the radio's cursor row stays above it — two layers of overlapping text are readable only if you already know what they say.
- **The no-second-port help says to power-cycle the radio.** He put the naive-user hat on deliberately and found the setting alone is not enough.
- **The two-finger blank actually works.** It was succeeding fewer than one try in ten, with the tune cursor stealing the gesture. Two fingers never leave the glass on the same instant, so a normal lift was measured as lasting until the *second* finger left — over the time limit nearly every time — and the finger left behind looked like a deliberate one-finger touch to both the pan handler and the tuning handler. ⚠ Not verified here; it needs two real fingers.

**Also documented (Stan Dye KC7XE):** in the band config table the Enable/Disable entries accept **E** and **D** as characters, and because those fields use the arrows to change the value, the arrows will not move you between columns while you are on one — step onto a numeric column first. That is the radio's own behaviour, and it explains something that otherwise reads as the arrows being broken in tables.

**Not a bug (Brian WA6JFK, answered by Roy KI0ER):** auto-answer is off after every restart by design. A radio that began transmitting the moment the Tab5 powered on might be feeding an antenna that is not tuned yet. WSJT-X requires arming transmit per startup for the same reason.

---

*Release-process note: this was an expedited cut. Everything safety-relevant ran - documentation both trees, the embedded manual rebuilt and committed before the tag, the binary's version read from its own descriptor, both build guards (standing patches, web-UI parse), and a real Tab5 flashed from this ZIP with the served page verified to parse. The full `build/` + `managed_components/` wipe was skipped for time; its main purpose was catching missing patches, which `tools/check_patches.py` now does mechanically on every build.*

