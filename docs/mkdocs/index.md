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
  distance and bearing, the furthest of the session, and a per-cycle history. Receiving is
  the default; transmitting is opt-in, with a duty cycle, optional band hopping, and
  **Protect finals** turning the radio's PA voltage down for the long key-down WSPR needs.
  See [WSPR](guide/wspr.md).
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
- **Updating from the device** *(needs WiFi)* — a new release is fetched quietly in the
  background and offered once, with **Restart now** or **Later**. Nothing is installed
  without you asking, and the automatic download can be switched off.
- **Built-in manual** — this whole guide is compiled into the firmware, so it is instant
  and needs no WiFi and no card. It opens at the chapter for the screen you are on,
  warning banners are tappable, and a **Need guidance?** panel takes your symptom in plain
  words. See [Getting Help](getting-help.md).
- **Time, on or offline** — SNTP *(needs WiFi)*, the Tab5's own supercap-backed RTC across
  power-off, the QMX's clock as an offline fallback, automatic GPS phase-lock if your QMX
  has one, a manual set-and-sync panel, and FT8 timing that self-corrects from the decoded
  band consensus.
- **microSD backup** — insert a card *before switching on* and your ADIF log, full config,
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

**v1.11.1 — a complete, self-contained FT8/FT4 station with no PC in the loop, a second
operating position in any browser, a WSPR propagation beacon, and the radio's own menus
on the screen.** The panadapter, FT8/FT4 receive and transmit, WSPR, ADIF logging and all
four logbook uploads — QRZ, eQSL, ARRL LoTW and your own Cloudlog or Wavelog — are stable
and in daily use.

**New in v1.11.1 — decoded CW along the bottom of the panadapter.** In CW or CW-R
the Morse the radio is decoding runs along the bottom of the waterfall, with an
estimate of the sending speed beside it, on the Tab5 and in the browser alike. The
QMX decodes it itself and hands the text over the CAT link, so it costs the
panadapter no processing and does not affect the spectrum or FT8 — and it works on
QMX firmware 1.03 and later, with nothing to enable on most radios. Noise is
filtered before it reaches the screen, the line wraps and overwrites itself rather
than scrolling, and the whole thing switches off in the settings drawer.

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

**Want the whole guide at once?** Download the [User Guide PDF](QMX-Panadapter-UserGuide-v1.11.1.pdf) — the whole user guide as one printable document.

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
