# Release announcement — v1.8.1 (draft for the operator to post)

Not sent. groups.io, QRPLabs, the Tab5 panadapter thread.

---

**QMX Panadapter v1.8.1 — the fixes from the first day of v1.8.0 in the field**

Nearly all of this came from reports on the day v1.8.0 went out. Thank you.

**The spectrum is tunable again wherever you can see it.** Most of the spectrum had
stopped responding to tap-to-tune — only a window in the middle worked, so it felt as
though tuning worked near the centre frequency and nowhere else, while the waterfall was
fine. The touch areas behind the top bar had been made shallower a week earlier without
the tune code being told, leaving a strip of screen that belonged to nobody. The rule now
is simply that if the mouse pointer is white, clicking tunes — and a finger behaves the
same.

**CW centre reaches the value your radio actually uses.** The radio offers 500 to 950 Hz
in 25 Hz steps and the slider offered 600 to 800 in 50 Hz steps, so several common
sidetones were unreachable. It also could never agree with the radio, because the Tab5
pushed its stored value about thirteen seconds before the CAT link exists. It now reads
the centre from the radio at connect. *(Samuel W7STF, Roy KI0ER)*

**The CW transmit offset puts VFO B back where it found it.** Switching the offset off did
return the radio to simplex, so FT8 was never transmitting off frequency, but VFO B was
left at your frequency plus the offset for the rest of the session. Your QMX may still
*show* both VFOs afterwards — that part is on the radio's side and is with QRP Labs;
switching band and back clears it. *(Roy KI0ER)*

**RF gain and volume agree between the Tab5 and the browser.** Whichever screen you opened
second used to show the value from before your change. *(Samuel W7STF)*

**Spot labels in the browser no longer swallow clicks meant for a signal.** A callsign can
be three or four kilohertz wide on screen, and clicking anywhere inside it took you to
that station instead of where you clicked. *(Samuel W7STF)*

**The seconds can be set again.** Hours and minutes were editable and the seconds were
not, so with no WiFi and no GPS there was no way to get the clock inside the second FT8
needs — which is exactly where Don was on a POTA activation. Hold the SS box in the FT8
Sync Time panel and let go on the minute. *(Don WB0LQW, gesture by Roy KI0ER)*

**The RIT button can be hidden** if you never use it: Settings → Radio → Show RIT button.
An offset that is actually engaged still shows itself. *(Samuel W7STF)*

Also: saving settings from the browser could fail with "HTTP 400" once the form grew past
a kilobyte, and the manual now says plainly that a Bluetooth mouse must be **BLE 4.0 or
later** — the Tab5's Bluetooth has no Classic radio, so an older Classic mouse never
appears at all and no firmware change can help.

**Two things this release does not fix.**

Bluetooth mouse decoding is unchanged. I found the real fault — the Tab5 was reading only
the first 22 bytes of the layout description every mouse publishes — but my fix broke
something else and I pulled it rather than ship it. It will be redone.

The QMX USB link still wedges occasionally after a long session, and only a power cycle of
the radio clears it. I tried six approaches from the Tab5 end, including holding USB power
off through a whole reboot. In that state the radio acknowledges the host's first question
and returns no data at all, so there is nothing more the Tab5 can do. It is with QRP Labs.

Download: https://github.com/SteffenLav/qmx-panadapter/releases/tag/v1.8.1
Unzip and run flash.bat (Windows) or flash.command (macOS/Linux). Settings, memory
channels, the QSO log and your LoTW certificate are all preserved.

Full detail: https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md

73 Steffen OZ1LAV
