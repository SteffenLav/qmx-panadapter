# QMX Panadapter

A standalone real-time panadapter — spectrum analyser and waterfall — for the [QRP Labs QMX/QMX+](https://www.qrp-labs.com/qmxp.html) HF transceiver.

Running on the [M5Stack Tab5](https://docs.m5stack.com/en/core/tab5) (ESP32-P4 with a 5" 720×1280 touch display), the panadapter connects to the QMX as a USB host, decodes the I/Q in real time, and renders a touch-driven interface with tap-to-tune, pinch-zoom, onboard FT8/FT4 decoding and transmit, ADIF logging, and a matching browser web UI.

**By Steffen Lav (OZ1LAV)**

## What it does

Everything below is in the firmware **today**. Nothing needs a PC; only the items marked
*(needs WiFi)* need a network.

- **Spectrum & waterfall** — a 48 kHz window at 30 Hz, adaptive per-bin noise floor,
  flat-spectrum mode, adjustable waterfall black level and contrast, selectable FFT
  window, a dBm-calibrated S-meter, and a colour band-plan strip that follows the VFO.
- **A spectrum that holds still** — from ×2 up, the spectrum and waterfall stay put and
  the VFO marker moves across them, so a signal stays where you last saw it and the
  waterfall's history lines up under the frequency it belongs to. The view re-frames only
  when your filter passband reaches the edge. The band above dial+12 kHz is hatched, because
  the radio genuinely cannot hear it. See [Panadapter](guide/panadapter.md).
- **Touch tuning** — tap or drag to tune with mode-aware snapping, pinch-zoom (a real
  zoom-FFT, not a stretch), one-finger pan-and-retune, edge-swipe navigation between
  screens, and a frequency keypad.
- **QMX control** — frequency, mode (USB/LSB/CW/DiGi, plus AM on QMX firmware 1.04+),
  SSB filter width, CW passband, TX power and SWR, and QMX volume in decibels matching
  the radio's own display. Band presets with per-band frequency recall, 32 memory
  channels, and **RIT** you set by tapping the caller you want to hear.
- **Radio menus** — the QMX's own 80×24 menu system on the Tab5 and in the browser, over
  the radio's *second* USB serial port, so the panadapter keeps decoding while you are in
  there. For a QMX+ with no control panel it is the only way in.
  See [Radio Menus](guide/radio-menus.md).
- **FT8 & FT4 receive** — continuous on-device decoding of both modes, in the same view
  as the panadapter. Callsign, country, signal report, slot-timing offset, audio tone,
  distance and bearing; include/exclude filters; band-aware worked-before exclusion; a
  pileup tracker; and the station you are working held at the top of the list.
- **FT8 & FT4 transmit** — tap a station to send the correct *next* message; call CQ from
  editable presets with an optional stop-after-N-calls limit; the full automatic exchange
  through to `73` and a log entry; a resend if your partner never heard your final; a
  polite hold for a station already working someone else; grey-listing; an optional
  unattended auto-answer robot; a drag-to-pick TX tone with a live occupancy strip, TX
  Hold and an ANY/EVEN/ODD time-window choice; and ARRL Field Day mode.
- **WSPR** — a third page, reached by the same swipe. A propagation beacon rather than a
  contact mode: a very slow, very weak signal carrying your callsign, grid and power,
  which stations worldwide report hearing. What was heard each two-minute cycle with band,
  distance and bearing, and the furthest of the session. Receiving is
  the default; transmitting is opt-in, with a transmit schedule and optional band hopping.
  **Calibrate Power** measures real RF output per band on a dummy load, so **Declared
  power** only offers standard dBm steps the radio actually reaches — labelled with the
  real wattage, not a textbook figure — which is also how you protect the finals for a
  long beacon run. A separate **Output power** slider covers every other mode. See
  [WSPR](guide/wspr.md).
- **Logging & upload** — every QSO written to an ADIF log on the device, readable and
  editable on the Tab5 or in the browser, and uploaded *(needs WiFi)* to **QRZ Logbook**,
  **eQSL**, **ARRL LoTW** and **your own Cloudlog or Wavelog** — LoTW QSOs signed on the
  device with your own certificate.
- **Activation logging** — start a **POTA** or **SOTA** activation and every contact is
  stamped with the reference as it is logged and counted against the threshold, with a
  single-reference ADIF export for uploading.
- **Live spots** *(needs WiFi)* — **POTA** activations, and optionally **RBN** CW skimmer
  spots and **DX cluster** spots, which are where phone activity comes from. Drawn on the
  trace where the station actually is, grey once you have worked them on that band, and one
  entry per station however many sources report it. Press and drag to pick one and lift to
  tune it *with the right mode*. See [Live Spots](guide/spots.md).
- **Knowing you are getting out** *(needs WiFi)* — **Who is hearing me** asks PSK Reporter
  which receivers copied *your* call, with distance, bearing and the report they gave you.
  **SWR protection** cuts a transmission short and latches the transmitter off above your
  chosen limit.
- **PSK Reporter** *(needs WiFi)* — the stations you decode are reported to the PSK
  Reporter map the way WSJT-X does. On by default, one checkbox to turn off, and never
  anything on the air.
- **Web UI** *(needs WiFi)* — the whole panadapter in a browser on the LAN: live spectrum
  and waterfall, click/drag/wheel tuning, band-mode-bandwidth-zoom control, the FT8/FT4
  band presets, live TX status with a **Call CQ** button, a sortable QSO log you can
  correct entries in, config download and upload, a microSD file browser, screenshots,
  and the diagnostic log.
- **Updating from the device** *(needs WiFi)* — tap the version line, press **Download
  now**, then **Restart now**. Nothing is fetched or installed until you ask.
  ⚠ **Coming from v1.14.x or earlier? v1.15.0 needed a one-time USB-C cable
  update** — do that first and everything after it is over the air. See
  [Settings](guide/settings.md#firmware-updates).
- **Built-in manual** — this whole guide is compiled into the firmware, so it is instant
  and needs no WiFi and no card. It opens at the chapter for the screen you are on,
  warning banners are tappable, and a **Need guidance?** panel takes your symptom in plain
  words. See [Getting Help](getting-help.md).
- **Time, on or offline** — SNTP *(needs WiFi)*, the Tab5's own supercap-backed RTC across
  power-off, the QMX's clock as an offline fallback, automatic GPS phase-lock if your QMX
  has one, a manual set-and-sync panel, and FT8 timing that self-corrects from the decoded
  band consensus.
- **microSD backup** — insert a card (restart afterwards) and your ADIF log, full config,
  LoTW certificate and key, and diagnostic log are mirrored automatically. Continuous with
  WiFi off (green SD dot); one complete backup per start-up with WiFi on (yellow dot).
- **Diagnostics** — an always-on log with nothing to enable: 5 MB in RAM, a rolling copy
  in flash that survives a power cut, and a full mirror to microSD. A crash survives the
  reboot and is reported on the next boot, so a diagnostic download is enough to answer
  an unexpected restart. Downloadable from the browser or over USB serial.
- **Practice mode** — phantom stations that call and reply through the real decode
  pipeline, so you can rehearse a whole QSO with **no radio connected** — with a hard
  interlock that never keys an attached QMX.
- **Keyboard & mouse** — the M5Stack Tab5 snap-on keyboard, attachable at any time, which
  drives the radio's menus and carries Ctrl shortcuts you can reassign. A **Bluetooth
  keyboard** types into every field (US layout only). A **Bluetooth mouse** works *while
  the QMX stays plugged in*, and its wheel tunes over the spectrum. USB mouse supported
  with the radio unplugged.
- **Settings you decide the shape of** — every setting on the Tab5 and in the browser,
  with a **Basic** and an **Advanced** view whose contents you choose from the web UI.
- **Field-ready** — WiFi entirely optional, and a static address if you want a browser
  bookmark to keep working. Battery percentage with a charge limit, display sleep, a
  180-degree flip for awkward mounting, config backup and restore as a text file, and a
  settings reset that needs no reflash.

## Status

**v1.15.1 — a complete, self-contained FT8/FT4 station with no PC in the loop, a second
operating position in any browser, a WSPR propagation beacon, and the radio's own menus
on the screen.** The panadapter, FT8/FT4 receive and transmit, WSPR, ADIF logging and all
four logbook uploads — QRZ, eQSL, ARRL LoTW and your own Cloudlog or Wavelog — are stable
and in daily use.

!!! warning "Coming from v1.14.x or earlier? One USB-C cable update first"

    **v1.15.0** reclaimed 2.81 MB of unused flash, which meant rewriting the
    partition table — something an over-the-air update cannot do. If you are
    still on v1.14.x or earlier you need that one **flasher download**, over
    the same USB-C cable you used the first time. **Everything from v1.15.0
    onwards, including this release, is over the air again** with more than
    twice the room. **Your settings, QSO log and LoTW certificate are kept** —
    press **Enter** at the flash-type prompt, never **E**. New users are
    unaffected, and anyone already on v1.15.0 simply updates from the device.

**v1.15.1 gives WSPR more working memory.** The routine that picks signals out of a cycle needs a large block of it, and on a busy page it could be left with only just enough — so a cycle could come back empty with traces plainly visible on the waterfall, most often after moving between pages. Freeing 2.8 MB removes the squeeze. The **Diagnostic download** is now a single zip file you can attach to an email, and it no longer clears the log when a transfer fails. **FT8, the SelfSpotter list and WSPR share one column order** so the same information sits in the same place on every screen.

**v1.14.4 fixed the reboots.** Fifteen runs on the bench before the fix lasted a median
of 15 minutes; with it, the same bench ran 14.7 hours with none.

**v1.14.1 fixed a v1.14.0 crash on tuning or QMX power-on.** A settings copy too
large for a 4096-byte task stack, hit by nearly any frequency change. Thanks to Martin
Howard and Rick Trommer W5NR for the reports, and Uwe DL8UG for the fix.

**New in v1.14.0 — Calibrate Power.** Sweeps *Max. PA voltage* through 45 points on a
dummy load and measures the real RF output at each, per band. **Declared power** (WSPR)
now only offers the standard dBm steps a band's calibration actually reaches, each one
labelled with the real measured wattage rather than a textbook figure computed from the
step's own name — so a declared level and what actually goes out can no longer disagree.
The old fixed "hold the PA at 6 V for the whole beacon" guard is gone; protecting the
finals is now just picking a low declared level, with a plain warning above 1 W. A
separate **Output power** slider covers every other mode and stays off the WSPR page,
since Declared power always owns the radio there. Both grow a **Recalibrate this band**
button once calibrated. Also: **SelfSpotter's** callsign-to-country table rebuilt from an
authoritative cty.dat-style source (~580 hand-curated prefixes → ~4,100 generated ones,
Uwe DL8UG), plus a new **ISO** country-code column on the map's LIST tab.

**In v1.13.0 — SelfSpotter** *(Uwe DL8UG, who wrote the whole thing and sent it as
a patch)*, a full-screen map and list answering "who is hearing me right now" from PSK
Reporter, wsprnet, and RBN's own self-spot feed, opened from the settings drawer. It
ships off by default. Land is filled in, not just outlined — LVGL has no built-in
polygon fill, so this is a small scanline rasteriser of its own — with the coastline
stroked back on top in its own colour so it doesn't disappear into the fill. A
deterministic **"Power-cycle QMX"** sequence *(Randy N4OPI)* for the web UI's remote
relay: pulse off, wait, pulse on, wait, then confirm over CAT rather than assume it
worked. Diagnostic log volume cut from roughly 300 lines a minute to under 50 on a
quiet bench. And this manual's own "Need guidance?" panel caught up with seven real
features that had no way in from it at all — Radio Menus, Still Spectrum, FT8
Simulation Mode, SWR protection, Antenna Tune, RIT, and "Release radio" — plus two
guidance bugs and a stale description of how to reach the spot map, both fixed.

**In v1.12.4 — one bug, twelve releases old.** Tapping a spot after tuning away with
the dial moved the spectrum and waterfall several kHz away from where the spot line
itself stayed, reachable only above ×1 zoom. Fixed.

**In v1.12.3 — the web page stops freezing, and WSPR stops reaching into your
other modes.** Writing the microSD card holds the spectrum stream, and on this
hardware that write can take a very long time — measured at 3, 10, 15 and once
29 seconds for four kilobytes, which is what made the web page look broken while
the spot list carried on updating. Card writes now wait while a browser is
actually watching, and go through anyway after three minutes so a crash still
reaches the card. On **WSPR**, a band-hop list kept hopping after you left the
page and wrote WSPR frequencies to the radio in the middle of an FT8 session, and
the reduced transmit voltage WSPR sets for its long key-down was restored with a
single unchecked command — so if that was lost, everything afterwards transmitted
at about a watt. Both fixed. Returning to the WSPR page now says "waiting for the
next cycle" instead of showing a frozen waterfall, the settings drawer sits above
the decoded-CW line, and the QSO log opens showing contacts.

**In v1.12.2 — the CW transcript reaches the microSD card at last, and WSPR stopped
costing the receiver its own audio.** Switch the decoded-CW transcript on, work a session,
pull the card, and `cw-decode.txt` held almost nothing: the code that writes it only ever
ran in the few seconds after boot, before WiFi came up — and **v1.12.1 "fixed" that by
adding a warning saying it was not happening.** It now writes every 30 seconds, a failed
write no longer throws the characters away, and a restart no longer welds two sessions
onto one line *(the fix is Uwe DL8UG's, sent as a patch)*. The card also stops giving up
for the rest of the session after three failed writes. On **WSPR**, the decoder was
running on the same processor core as the USB audio service and losing up to 1.4 % of the
audio the radio had already sent — more than a whole symbol of drift across a
transmission, and enough to stop a cycle decoding at all; it now runs on the other core.
The waterfall shows three minutes, every cycle is labelled, and letters appear as each
line decodes. The Tab5 wakes on the page it was left on, and the manual's notes and
warnings render properly again in the printable guide and on the device.

**In v1.12.1 — letters on the WSPR waterfall, and a Band button that no longer
retunes your radio.** Every trace the decoder looks at is lettered A, B, C… left to right
by tone, matched by a new first column in the decode list; a trace it could not read is
marked `?`. On the WSPR page the top bar's Band control was sitting over the page's own
band panel and winning the touch, so picking a band there wrote an FT8 frequency to the
radio while WSPR carried on labelling spots with the old one. An FT8 reply can now make
its own slot instead of waiting for the whole band to finish decoding, a band change
during a transmission is held rather than lost, and the remote power-switch modification
is documented in the manual with a schematic, by Randy N4OPI.

**In v1.12.0 — CW profiles.** The QMX holds one CW centre and one set of filter
widths, and changing them means walking two separate menus on the radio. Four profiles
now live on the Tab5 — a name, a centre frequency, and which of the eight filter widths
to offer with it — edited on the web settings page and applied from there or from a
picker in the settings drawer. Applying one writes the radio's own configuration, so
every write is read back and retried rather than assumed. The bandwidth list also asks
the radio which filters it actually has, instead of offering all eight regardless.

**A CAT link could die and go on reporting itself healthy.** One transient USB error
could stop the Tab5 hearing the radio for good while the poll kept saying the link was
fine — so the screen asked you to restart a radio that was working perfectly. Caught in
a soak at four hours fifty-six minutes, with the link then dead for the following four.
A watchdog now forces a reconnect after five seconds of silence.

**WSPR** gains a DT column on both screens, distances that follow the km/miles setting
(they were always kilometres), a Clear button for the decode list, one capture countdown
instead of two, hover-a-trace-to-name-the-station on the waterfall, and a band picker you
drag through rather than a dropdown that commits wherever your finger lifts.

**Tune snap can be switched off** — Off / 250 Hz / 500 Hz / 1 kHz, defaulting to the
present behaviour. And on the web page, the spectrum above ×1 zoom had no frequency scale
at all, which is why clicking a signal tuned to the wrong place, the passband sat
elsewhere than on the Tab5, and the band-plan slider came apart as it was dragged. Every
field in a QSO can now be corrected, not four.

**In v1.11.1 — decoded CW along the bottom of the panadapter.** In CW or CW-R the
Morse the radio is decoding runs along the bottom of the waterfall, with an estimate
of the sending speed beside it, on the Tab5 and in the browser alike. The QMX decodes
it itself and hands the text over the CAT link, so it costs the panadapter no
processing and works on QMX firmware 1.03 and later.

**In v1.10.5 — the spectrum holds still while you tune across it.** The panadapter now
behaves the way a Flex does: the spectrum and waterfall stay where they are and the VFO
marker moves over them, so a signal stays put on screen while you tune towards it and the
waterfall's history stays lined up under the frequency it belongs to. The view re-frames
only when you tune far enough to need it, and what triggers it is your filter passband
reaching the edge of the screen rather than a fixed percentage — so it feels the same in
every mode. On by default above ×1; at ×1 the display stays centred on the dial, because
the view is already the whole 48 kHz the radio sends.

**The right-hand quarter of the ×1 view was showing real signals at the wrong frequency.**
The QMX's local oscillator sits 12 kHz below the dial, so there is no data above dial+12
kHz — and the display was filling that quarter by wrapping the bottom of the band into it,
with the frequency scale labelling it as dial+12 to +24. Tapping a signal there tuned you
about 48 kHz away from it. That region is now hatched and inert on both screens.

**From the groups.io reports:** changing band ends a contact in progress instead of
carrying the call sequence onto the new band, and the WSPR transmit button has moved clear
of the left edge where a swipe could catch it *(both Randy N4OPI)*; WSPR spots show the
band they were heard on *(Roy KI0ER)*; the WSPR waterfall marks a transmit cycle instead
of leaving the previous picture looking frozen *(Dirk)*; the browser gets the FT8/FT4 band
preset list; a LoTW upload shows LoTW's own reply rather than only our count of what was
sent; the LoTW certificate can be replaced from a visible button; and the Tab5 can be
given a static IP address.

**Every earlier release** is on the [Releases](releases.md) page, and in full detail in
[version-history.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md).

## Get Started

**New user?** Start with the [Quick Start](quick-start.md) guide — 10 minutes to on-air.

**Stuck, or not sure what something is called?** The Tab5 can help you itself — see [Getting Help](getting-help.md).

**Want the whole guide at once?** Download the [User Guide PDF](QMX-Panadapter-UserGuide-v1.15.1.pdf) — the whole user guide as one printable document.

**Builder?** Head to [Build from Source](build/build.md) for ESP-IDF setup and the complete module map.

## The QMX Connection

The QMX exposes two USB interfaces:

| Interface | Data |
|-----------|------|
| **UAC** (USB Audio Class) | I/Q stereo audio, 48 kHz, 24-bit |
| **CDC-ACM** (serial) | Kenwood-style CAT commands (FA, MD, FW, TX, RX, etc.) |

The Tab5 connects as a **USB host**, receiving both streams over a single USB-A to USB-C cable. No drivers needed on the Tab5 — they're built into ESP-IDF.

## Quick Links

- **[GitHub Repository](https://github.com/SteffenLav/qmx-panadapter)** — source code, releases, issue tracking
- **[QRP Labs QMX Manual](https://www.qrp-labs.com/qmx.html)** — radio specs and CAT reference
- **[M5Stack Tab5 Docs](https://docs.m5stack.com/en/core/tab5)** — hardware documentation

---

*QMX and QMX+ are products of [QRP Labs](https://www.qrp-labs.com). Tab5 is a product of [M5Stack](https://m5stack.com).*
