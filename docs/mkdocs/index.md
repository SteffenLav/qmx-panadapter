# QMX Panadapter

A standalone real-time panadapter — spectrum analyser and waterfall — for the [QRP Labs QMX/QMX+](https://www.qrp-labs.com/qmxp.html) HF transceiver.

Running on the [M5Stack Tab5](https://docs.m5stack.com/en/core/tab5) (ESP32-P4 with a 5" 720×1280 touch display), the panadapter connects to the QMX as a USB host, decodes the I/Q in real time, and renders a touch-driven interface with tap-to-tune, pinch-zoom, onboard FT8/FT4 decoding and transmit, ADIF logging, and a matching browser web UI.

**By Steffen Lav (OZ1LAV)**

## What it does

Everything below is in the firmware **today**. Nothing needs a PC; only the items marked
*(needs WiFi)* need a network.

- **Spectrum & waterfall** — a 48 kHz window centred on the VFO at 30 Hz, adaptive
  per-bin noise floor, flat-spectrum mode, adjustable waterfall black level and contrast,
  selectable FFT window, a dBm-calibrated S-meter, and a colour band-plan strip that
  follows the VFO.
- **Touch tuning** — tap or drag to tune with mode-aware snapping, pinch-zoom (a real
  zoom-FFT, not a stretch), one-finger pan-and-retune, edge-swipe navigation between
  screens, and a frequency keypad.
- **QMX control** — frequency, mode (USB/LSB/CW/DiGi, plus AM on QMX firmware 1.04+),
  SSB filter width, CW passband, TX power and SWR, and QMX volume in decibels matching
  the radio's own display. Band presets with per-band frequency recall, and 32 memory
  channels holding any frequency and mode.
- **FT8 & FT4 receive** — continuous on-device decoding of both modes, in the same view
  as the panadapter. Callsign, country, signal report, slot-timing offset, audio tone,
  distance and bearing; include/exclude filters; band-aware worked-before exclusion; a
  pileup tracker; and the station you are working held at the top of the list.
- **FT8 & FT4 transmit** — tap a station to send the correct *next* message; call CQ from
  editable presets with an optional stop-after-N-calls limit; the full automatic exchange
  through to `73` and a log entry; a resend if your partner never heard your final; a
  polite hold for a station already working someone else; grey-listing; an optional
  unattended auto-answer robot; a drag-to-pick TX tone with a live occupancy strip, TX
  Hold and an EVEN/ODD time-window choice; and ARRL Field Day mode.
- **Logging & upload** — every QSO written to an ADIF log on the device, readable and
  editable on the Tab5 or in the browser, and uploaded *(needs WiFi)* to **QRZ Logbook**,
  **eQSL** and **ARRL LoTW** — LoTW QSOs signed on the device with your own certificate.
- **Live spots** *(needs WiFi)* — **POTA** activations, and optionally **RBN** CW skimmer
  spots and **DX cluster** spots, which are where phone activity comes from. Drawn on the
  trace where the station actually is, grey once you have worked them on that band, and one
  entry per station however many sources report it. Press and drag to pick one and lift to
  tune it *with the right mode*. See [Live Spots](guide/spots.md).
- **PSK Reporter** *(needs WiFi)* — the stations you decode are reported to the PSK
  Reporter map the way WSJT-X does. On by default, one checkbox to turn off, and never
  anything on the air.
- **Web UI** *(needs WiFi)* — the whole panadapter in a browser on the LAN: live spectrum
  and waterfall, click/drag/wheel tuning, band-mode-bandwidth-zoom control, live FT8 TX
  status with a **Call CQ** button, a sortable QSO log, config download and upload, a
  microSD file browser, screenshots, and the diagnostic log.
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
  in flash that survives a power cut, and a full mirror to microSD. Downloadable from the
  browser or over USB serial.
- **Practice mode** — phantom stations that call and reply through the real decode
  pipeline, so you can rehearse a whole QSO with **no radio connected** — with a hard
  interlock that never keys an attached QMX.
- **Field-ready** — WiFi entirely optional, battery percentage and voltage with a charge
  limit, display sleep, a 180-degree flip for awkward mounting, config backup and restore
  as a text file, and optional snap-on keyboard and USB mouse support.

## Status

**v1.8.3 — a complete, self-contained FT8/FT4 station with no PC in the loop, a second
operating position in any browser, and now a mouse.** The panadapter, FT8/FT4 receive and
transmit, ADIF logging and all three logbook uploads are stable and in daily use.

**New in v1.8.3 — a field-report release: five reported faults, all found by users.** You can now **change your QRZ or eQSL login** from the browser — before this the prompt only ever appeared when nothing was stored, so a mistyped key could not be replaced at all (Brian WA6JFK). The **dB scale labels follow the range you set** instead of being fixed at −40 to −120, and the **Adaptive floor slider is gone** because it could never change anything. The **filtered part of the spectrum is drawn where the radio actually filters** — 150 to 3200 Hz in the digital modes, not 200 to 2900. **RF gain no longer sticks on "reading…"**. The **dark bands at the edges of a zoomed view are about half as wide**. Out of band the **band strip becomes a coarse tuner** you drag off centre to move the dial. And the browser's decode list gained **distance and bearing** columns (Tony Abbey). Most of this came from Samuel W7STF.

**New in v1.8.2 — your radio's own spurs can be removed from the display, and a POTA clock that stopped being stolen.** If you have ever seen evenly spaced signals that never move and are still there with the antenna unplugged, those come from the QMX's own synthesizer — **spur suppression** finds them by nudging the dial 25 Hz and can subtract or erase them, **off by default** under Settings → Waterfall. A **QMX without GPS no longer overwrites an accurate clock** when you switch it on at a POTA site; the Tab5's own RTC wins and sets the radio instead. **RIT can be parked** with a long press and restored unchanged, and its offset is now printed beside its marker on the waterfall. The **band strip stays visible out of band**. And the panadapter no longer **switches your radio off** trying to recover a fault it cannot fix.

**New in v1.8.1 — the fixes from the first day of v1.8.0 in the field.** Most of the spectrum had stopped responding to **tap-to-tune**; the rule now is that if the mouse pointer is white, clicking tunes, and a finger behaves the same. **CW centre** covers the radio's real 500–950 Hz in 25 Hz steps and is read from the radio at connect. The **CW transmit offset** puts VFO B back where it found it. **RF gain and volume** agree between the Tab5 and the browser. Browser **spot labels** no longer swallow clicks meant for a signal kilohertz away. The **seconds are settable again** — hold the SS box and release on the minute, the only way to set the clock with no WiFi and no GPS. And the **RIT button can be hidden**.

**New in v1.8.0 — RIT you set by tapping, summit spots, and a browser that finally matches the Tab5.** Arm **RIT** and tap a caller to receive off your transmit frequency, with a marker showing where you are listening. **SOTA spots** join POTA, RBN and the DX cluster. **Fox/Hound** works DXpeditions from the hound side, simulation-verified so far. And the browser gained RIT, activation start/stop, the last Tab5-only settings, and a spectrum and waterfall drawn the way the Tab5 draws them.

⚠ **If you use the browser, two things are worth checking.** **SWR protection set from the browser was never saved** on any v1.7.x build — check it on the Tab5 under Settings. And **spot labels were stealing clicks** near their own callsign. Both fixed.

**In v1.7.0 — a mouse, the phone spots that were missing, and knowing who hears you.**
A **Bluetooth mouse** drives the Tab5 *while the QMX stays plugged in* — the case a USB
mouse can never serve, because the radio owns the only USB port. Pair it once and it
reconnects by itself; the wheel scrolls whatever is under the pointer. **DX cluster spots**
add the SSB activity RBN structurally cannot see, because skimmers are machines and no
machine recognises a callsign spoken into a microphone. **Activation mode** stamps every
contact with your park or summit, counts them against the threshold, and can export that
one reference on its own. **SWR protection** cuts a transmission short and latches off if
the antenna is wrong. And **who is hearing me** asks PSK Reporter which receivers copied
*your* call — the only way to tell a dead band from a transmit-side fault.

Every release, newest first, is on the [Releases](releases.md) page.

## Get Started

**New user?** Start with the [Quick Start](quick-start.md) guide — 10 minutes to on-air.

**Stuck, or not sure what something is called?** The Tab5 can help you itself — see [Getting Help](getting-help.md).

**Want the whole guide at once?** Download the [User Guide PDF](QMX-Panadapter-UserGuide-v1.8.3.pdf) — the whole user guide as one printable document.

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
