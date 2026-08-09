# QMX+ Panadapter

*By Steffen Lav (OZ1LAV).*

A standalone real-time panadapter — spectrum analyser and waterfall — for the [QRP Labs QMX/QMX+](https://www.qrp-labs.com/qmxp.html) HF transceiver, running on the [M5Stack Tab5](https://docs.m5stack.com/en/core/tab5) (ESP32-P4 with a 5" 720×1280 touch display).

The QMX exposes I/Q audio over USB UAC plus CAT control over USB CDC-ACM. The Tab5 connects to the QMX as a USB host, decodes the I/Q in real time on the ESP32-P4, and renders a touch-driven panadapter with tap-to-tune, pinch-zoom, onboard FT8/FT4 decoding and transmit, ADIF logging, and a matching browser web UI.

**Documentation:** [tab5.lav.dk](https://tab5.lav.dk) — the user guide, quick-start, and reference as plain web pages. A more approachable read than this page if you just want to set the device up; the source code and release downloads stay here on GitHub.

![Panadapter on M5Stack Tab5 — QMX+ tuned to 14.074 MHz, FT8/FT4 traffic visible](docs/QMX-Panadapter_v0.9.2.png)

*20 m FT8 pile-up around 14.074 MHz in flat-spectrum mode (v0.9.2). The spectrum trace tracks a per-bin noise floor so real signals pop sharp above a calm baseline. Top bar: band, mode, centre freq, S-meter. Bottom bar: battery, WiFi strength, IP. The same view streams live to any browser on the LAN — see [Web UI](#web-ui).*

> **Release — v1.6.0.** A complete, self-contained FT8/FT4 station: spectrum and waterfall, on-device decode and transmit, automatic QSOs, ADIF logging, and upload to **all three major logbooks — QRZ, eQSL and ARRL LoTW** — with no PC in the loop. It runs offline for POTA/SOTA, streams to any browser on the LAN, and carries its own user manual inside the firmware.
>
> **New in v1.6.0 — the browser became a second operating position.** The web page could show you the band; now it can work it. Tap a station to **reply** (the same decision the Tab5 makes about which message comes next, with a confirmation first), pick your **TX tone** against a live occupancy strip, edit **settings** and **memory channels** on a real keyboard, and read the **whole manual** — all served by the Tab5 itself, no internet. It now answers to **`qmx.local`**, so its IP stops mattering. For CW there is a **transmit offset** so a QRP call is not buried in the zero-beat pile (Roy KI0ER), plus **RF gain** and a **Release radio** button for using the QMX's own menus without the two fighting over the port (Stan KC7XE). A radio that stops sending audio now **recovers itself**. The manual gained an **A–Z index** and proper **drawn diagrams** on both the Tab5 and the website, and the settings drawer is **grouped with a Basic/Expert toggle**. Full detail in [docs/version-history.md](docs/version-history.md).
>
> **What changed in earlier releases** is in **[docs/version-history.md](docs/version-history.md)** — every release from v0.1.0 onward, newest last. The section below describes what the firmware does **today**, not what any one release added.

Prefer a single printable file? [Download the User Guide PDF](docs/QMX-Panadapter-UserGuide-v1.6.0.pdf).

<!-- USERGUIDE:START -->

The full documentation also lives online at **[tab5.lav.dk](https://tab5.lav.dk)** — the user guide, quick-start, and reference as searchable, cross-linked web pages, kept in sync with every release.

---

## Features

Everything below is in the firmware **today**. Nothing here needs a PC, and only the
items marked *(needs WiFi)* need a network.

**Real-time panadapter** — Spectrum and waterfall across a 48 kHz window centred on the
QMX VFO, at 30 Hz, with 12 kHz IF-offset compensation. Tap or drag to tune (mode-aware
snapping), pinch to zoom ×1–×8 with a true zoom-FFT, one-finger drag to pan and retune.
Adaptive per-bin noise floor, flat-spectrum mode, adjustable waterfall black level and
contrast, selectable FFT window, and a graphical S-meter calibrated in dBm. A colour
band-plan strip tracks the VFO (IARU region 1/2/3, auto-selected from your grid) and can
be dragged to scrub across the band.

**QMX CAT control** — Live frequency, mode (USB/LSB/CW/DiGi, plus AM on QMX firmware
1.04+), SSB filter bandwidth, CW passband, passband overlay, TX power and SWR readout,
QMX volume in decibels matching the radio's own display, and an Antenna Tune button on
1.04+. Round-trip CAT latency under 50 ms. Band presets with per-band frequency recall,
and 32 memory channels in a 4×8 grid holding any frequency and mode.

**FT8 and FT4, receive** — Continuous on-device decoding in the same view as the
panadapter: FT8 (15 s slots) and FT4 (7.5 s slots). The decode list shows callsign,
country, signal report, slot-timing offset (DT), audio tone (HZ), distance and bearing,
with the station you are working held at the top. Include/exclude filters match anything
in the message text, worked-before stations can be excluded band-by-band, and a pileup
tracker collects everyone calling you.

**FT8 and FT4, transmit** — Tap a station and the correct *next* message is sent
(WSJT-X double-click style); or call CQ from one of three editable presets, with an
optional stop-after-N-calls limit. Full automatic exchange through to `73` and an ADIF
entry, a resend if your partner never heard your final, a polite hold for a station
already working someone else, grey-listing for stations that never answer, and an
optional unattended auto-answer robot. Your TX tone is a permanent on-screen button with
a live occupancy strip, drag-to-pick tone selection, **TX Hold**, and a cycling
**TXCQ ANY / EVEN / ODD** time-window choice. ARRL Field Day exchange mode included.

**QSO logging and upload** — Every contact is written to an ADIF log on the device, with
timestamp, callsign, grid, frequency, band, mode, both signal reports and distance
(never a report that was not actually exchanged). Read the log on the Tab5 or in the
browser, delete single records or all of them, and upload *(needs WiFi)* to **QRZ
Logbook**, **eQSL** and **ARRL LoTW** — LoTW QSOs are signed on the device itself with
your own callsign certificate.

**Live spots on the spectrum** *(needs WiFi)* — **POTA** park activations, and
optionally **RBN** CW skimmer spots, drawn onto the trace at the frequency the station
is actually using. Grey means you have already worked them on that band. Press and drag
to pick one, lift to tune it *with the right mode*. Spots fade with age and are gone
after 30 minutes; corner counts take you to the ones just off-screen.

**PSK Reporter spotting** *(needs WiFi)* — The stations you decode are reported to
[PSK Reporter](https://pskreporter.info) the way WSJT-X does, so you appear on the map as
a monitoring station. **On by default**; sends your call and grid plus each decoded
station's call, grid, frequency, report and mode, batched at most once every five
minutes — over the internet only, **never on the air**. One checkbox turns it off, and it
is inert until your callsign and grid are set, and in simulation mode.

**Web UI** *(needs WiFi)* — The whole panadapter in any browser on the LAN: live
spectrum and waterfall at ~10 fps, click or drag to tune, mouse-wheel pan and tune,
band/mode/bandwidth/zoom control, a graphical S-meter and the whole-band plan strip. In
FT8/FT4 mode it shows a live TX status banner and a **Call CQ** button instead of the
stream. Also: the QSO log as a sortable table, config download/upload as an editable text
file, a microSD file browser, screenshots, and the diagnostic log.

**Built-in user manual** — This entire guide is compiled into the firmware, so it opens
instantly with no WiFi, no card and no download, and can never describe a different
version than the one you are running. It opens at the chapter for the screen you are on,
warning banners are tappable, and a **Need guidance?** panel lets you pick your symptom
in plain words. See [Getting help](#getting-help).

**Time, with or without a network** — SNTP when WiFi is up *(needs WiFi)*, the Tab5's own
supercap-backed RTC across power-off, the QMX's clock as an offline fallback, GPS
detected and phase-locked automatically if your QMX has one, and a manual set-and-sync
panel. FT8/FT4 timing also self-corrects from the decoded band consensus when offline.

**microSD station backup** — Insert a card (a plain FAT32 32 GB card is ideal) **before
switching on** and your whole station is mirrored to `/qmx-panadapter/`: the ADIF log, a
full config export, your LoTW certificate and key, the diagnostic log and a
self-describing `README.txt`. Continuous with WiFi off (green **SD** dot); one complete
backup per start-up with WiFi on (yellow dot), because the card and the WiFi
co-processor share a bus. *(The card holds credentials — WiFi password, QRZ/eQSL logins,
LoTW private key — so keep it physically secure.)*

**Diagnostics** — An always-on diagnostic log, nothing to enable: 5 MB in RAM, a rolling
copy in flash that survives a power cut, and a full mirror to microSD if a card is in.
Downloadable from the browser or over USB serial. Bug reports become answerable.

**Practice mode** — A simulator with phantom stations that call CQ and reply through the
real encode/decode pipeline, so you can rehearse a full QSO with **no radio connected**
— and with a hard interlock that never keys a QMX even if one is attached.

**Touch, keyboard, mouse** — Edge-swipe navigation with breathing grip handles, an
on-screen keyboard, optional support for the M5Stack Tab5 70-key snap-on keyboard, and
USB mouse support. *(The mouse and the QMX cannot share the single USB host port — the
ESP32-P4's USB stack lacks the Transaction Translator a hub would need — so the mouse is
for setup, log review and reading the manual with the radio unplugged.)*

**Built for the field** — WiFi is entirely optional; battery percentage and voltage with
a charge limit for battery care; display sleep and a 180° flip for awkward mounting;
config backup and restore as a text file; and a settings reset that does not need a
reflash.

---

## Contents

- [Quick Guide](#quick-guide) — get on air in 10 minutes
- [Getting help](#getting-help) — the manual on the device, and the guidance panel
- [Panadapter](#panadapter) — spectrum, waterfall, zoom, touch-to-tune, S-meter, memory channels
- [Web UI](#web-ui) — browser panadapter and remote control
- [FT8 Receive](#ft8-receive) — onboard decoder, decode list
- [FT8 Transmit](#ft8-transmit) — reply, CQ-run, auto-QSO, ADIF logging
- [Time sync](#time-sync) — WiFi/SNTP, Tab5 RTC, POTA/offline use
- [Reference](#reference) — gestures, settings drawer, web API, hardware
- [Build from source](#build-from-source)
- [Under the hood](#under-the-hood) — DSP, I/Q correction, quirks
- [Roadmap](#roadmap)
- [Related projects](#related-projects)

---

## Quick Guide

### Step 0 — Check your QMX firmware first

Power the QMX on by itself and read the firmware version off its own display at boot. You need **1.03.002 or newer**. If yours is older, update the QMX *before* connecting the Tab5 — everything that follows depends on it. This takes 10 seconds to verify and saves hours of debugging. Both **1.03.002** and the **1.04.002 beta** work with the panadapter; on 1.04.002 the Tab5 additionally offers **AM mode** and an **Antenna Tune** button (SWR tune with a live power/SWR readout) — both stay hidden on 1.03.002, so there's no downside either way.

### Step 1 — Flash the Tab5 firmware

Use the one-click flasher in [`tools/QMX-Panadapter flasher/`](tools/QMX-Panadapter%20flasher) (also attached to each [release](https://github.com/SteffenLav/qmx-panadapter/releases)):

1. Plug the Tab5 into your computer with a **USB-C data cable** — charge-only cables will not work.
2. Run the flasher:
   - **Windows** — double-click `flash.bat` (downloads esptool + firmware automatically; nothing to install)
   - **macOS** — double-click `flash.command` (needs esptool once — `brew install esptool` recommended; `pip3 install esptool` often fails on recent macOS with an "externally-managed-environment" error). If macOS blocks the script, right-click → **Open**, or run `bash flash.command` in Terminal (no `chmod` needed).
   - **Linux** — `bash flash.command` (needs `pip3 install esptool`)
3. The flasher asks **normal or clean** (see below) — just press **Enter** for a normal flash.
4. Wait for `SUCCESS`. The Tab5 restarts on the new firmware.

The flasher downloads the latest release from GitHub automatically. **No internet?** Put a `qmx_panadapter_merged_*.bin` from the [releases page](https://github.com/SteffenLav/qmx-panadapter/releases) next to the flasher and it uses that instead.

**Normal vs clean flash.** Just before flashing, the flasher asks:

> *Type E for a CLEAN/ERASE flash, or just Enter for a normal flash*

- **Normal (press Enter)** — updates the firmware and **keeps** all your saved settings (WiFi, callsign, grid, memory channels, ADIF log). This is what you want almost every time.
- **Clean (type E)** — wipes the whole chip first, so **every saved setting is permanently erased**: WiFi name *and* password, callsign/grid, all memory channels, and the logged QSOs. Use it only if something is stuck or corrupted (e.g. WiFi refuses to turn on no matter what). Back up first with **Config ↓** (below) so you can restore in seconds afterwards.

Building from source? See [Build from source](#build-from-source).

### Step 2 — Connect the cables

You need **two** USB connections, and the cable to the QMX is the one people get wrong:

| Connection | Port on Tab5 | Cable | Carries |
|------------|--------------|-------|---------|
| **Tab5 → QMX** | **USB-A** (host) | **USB-A → USB-C, full data cable** | I/Q audio (UAC) + CAT (CDC-ACM) |
| **Tab5 → power** | **USB-C** | Any USB-C power cable (5 V / 2 A+) | Power |

> **The #1 failure is a charge-only cable.** Many USB-C cables — especially thin ones bundled with phones and chargers — carry power only, no data lines. If you use one between the Tab5's USB-A port and the QMX, the Tab5 powers the QMX but sees no audio and no CAT: the spectrum stays flat and the top bar shows `Band: ---`. Use a cable you know does data (one that works for a USB stick or phone file transfer). When in doubt, swap the cable first.

**Power-on order matters.** Turn the **Tab5 on first** and let it finish loading, then turn the **QMX on**. Within a few seconds the top bar should populate Band / Mode / BW and the spectrum should come alive.

Once flashed you can power the Tab5 from any 5 V/2 A USB-C source or the internal battery — the laptop is only needed for flashing. For diagnostics the Tab5 also outputs a serial log over the USB-C data connection (useful if WiFi is not available), but for most users the built-in **diagnostic log** — always on, nothing to enable — is the easier path — see [Step 7](#step-7--something-not-working).

### Step 3 — Find your way around

The whole app is driven by **one-finger swipes from the screen edges** and **taps on the top bar**. The settings drawer is always one swipe away — no hunting for hidden menus.

| Gesture | From | Does |
|---------|------|------|
| Swipe → | Left edge | Toggle Panadapter ↔ FT8 screen |
| Swipe ← | Right edge | Open settings drawer |
| Swipe ↑ | Bottom edge | Open memory channel picker |
| Tap or swipe ↓ | Any top-bar item | Open that item's selector |

Slim "breathing" grip handles on each edge show where to swipe — faint enough not to clutter the display, visible if you look for them.

The top bar reads **Band · Mode · BW · Freq · Signal · Zoom** left to right. Tap any item to open a selector — **Freq** opens the frequency keypad; **Mode** switches USB/LSB/CW/DiGi; **BW** selects SSB filter width (USB/LSB only); **Band** jumps to a configured band; **Zoom** selects a zoom preset.

See the [full gesture table](#gestures) in the Reference section.

### Step 3b — Choose FT8 or FT4 mode

FT8 is available on all HF bands; FT4 is available on select bands with published frequencies (20 m, 30 m, 40 m, etc.). To switch between them:

1. **In Panadapter mode:** Tap the **Mode** selector in the top bar, then choose **DiGi** (the digital mode group).
2. **In FT8 screen:** Tap the **Preset** frequency button to open the band/mode picker — you'll see **two columns**: FT8 (gold header) on the left, FT4 (cyan header) on the right. Tap any frequency in the FT4 column to switch modes.

The decode list, TX controls, and slot timing (countdown bar, which-parity-next) all adapt automatically to the active mode. Switching modes clears stale decodes from the previous mode so you don't work old QSOs.

**When to use each mode:**
- **FT8** — Most populated band in SOTA/POTA; longer decode window gives more time to prepare replies on slow links or weak signals.
- **FT4** — Faster QSO cycle for busy bands (Field Day, major contests, 20 m pile-ups) and operators who prefer shorter waits between exchanges.

### Step 4 — Fill in your settings

**On first boot the Tab5 opens a setup prompt automatically** — it will ask for your callsign, grid square, and WiFi credentials before you reach the main screen. You can skip any field and fill it in later via the settings drawer.

To open the settings drawer at any time: swipe in from the right edge, or tap the right grip handle.

- **WiFi** — enter your SSID and password. Recommended for everyone: you get accurate UTC time (needed for FT8 slot timing), the browser web UI, and remote diagnostics. The **WiFi initiated** checkbox below the WiFi section is an on/off master switch — uncheck it for POTA or field use with no network.
- **Identity** (callsign + grid square) — *only required for FT8/FT4 transmit*. Enter your callsign and Maidenhead grid (4 or 6 characters, e.g. `JO45` or `JO45ab`). The grid also drives the distance and bearing columns in the FT8 decode list.

Everything else — dB range, smoothing, colour map, brightness, IQ balance — has sane defaults. Adjust if you want to, but you don't need to on first use.

### Step 5 — Using the panadapter

Switch the QMX to any band and watch it come alive. This is the foundation of the device regardless of which modes you operate.

**Tap or drag to tune.** Touch anywhere on the spectrum or waterfall to place the cyan cursor. Drag and it snaps to a mode-aware grid — 10 Hz steps in CW, 250 Hz in SSB, 500 Hz for FT8/FT4 — so you can land precisely on a signal before lifting your finger. Lift, and the QMX retunes.

**Swipe fast to pan instead.** A quick horizontal swipe (rather than a held drag) slides the whole view left or right and retunes to wherever you let go — the fastest way to slide along a band. See [Touch-to-tune](#touch-to-tune) for exactly how the two gestures are told apart.

**Or move precisely with the band-plan slider.** For a controlled move rather than a quick swipe, drag the framed "visible window" on the band-plan strip (along the bottom) to exactly where you want to be on the band — the spectrum, waterfall, and VFO all follow it, so you can place yourself precisely on a segment before you even tap a signal.

**Zoom in on a crowded band.** Pinch with two fingers to zoom up to ×24. On a CW band at ×8 or higher you can resolve individual signals a few hundred Hz apart, read the spacing between callers, and pick your target before you tune. The frequency axis scales with you, down to Hz precision at high zoom. Double-tap anywhere to reset to the full 48 kHz view.

**Find your place on the band.** The thin band-plan strip along the bottom of the screen (just above the status bar) colour-codes the CW/Digi/Phone segments around you — pick your IARU region (or leave it on Auto) in the settings drawer → **Band-plan region**. The framed window on the strip is a **slider**: drag it (or tap anywhere on the strip) to move precisely to any spot on the band, and the spectrum, waterfall and VFO all follow along. You can also grab that slider from anywhere along the bottom status bar and drag sideways — a taller target — while a vertical up-swipe there still opens memory channels.

**Read your passband.** Two grey vertical lines on the spectrum mark your current filter edges. The BW label in the top bar shows the active width; tap it to choose 2.5 / 2.7 / 2.9 / 3.2 kHz in USB or LSB. A coloured tint fills the passband so you can always see exactly what slice of the band you're receiving.

**Watch the S-meter.** The Signal field in the top bar is a live tick-scale bar (S1 through S9+20), driven by the actual signal under your VFO cursor.

**Flat-spectrum mode.** Toggle **Flat spectrum** in the settings drawer to switch from absolute dBm to dB-above-local-noise-floor. Noise collapses to a calm baseline; real signals — including weak CW tones in the mud — pop sharply above it. Recommended on noisy bands.

**Save your spots.** Swipe up from the bottom edge to open the memory channel picker — a 4×8 grid of 32 channels. It doesn't greet you with blank cells: a fresh device arrives with a handful of **example channels already filled in**, and each one is **colour-coded by mode** (CW green, DiGi teal, USB brick-red, LSB purple) so you can read the whole grid at a glance without opening a single label. The very first time you open it, a quick **guided animation** walks you through the gestures — watch a channel slide to a new slot, then drop into the wastebin and vanish — so you learn "drag to move, drag to delete" in about ten seconds, once, and never again.

Then it's yours: **tap** a channel to recall it (the QMX retunes instantly), **tap an empty slot** to store the current frequency/mode, **long-press** to edit, **drag** a channel to reorganise the grid, and **drag onto the wastebin** (bottom-right cell) to delete it.

**Monitor from another room — or operate remotely.** Once WiFi is configured, open `http://<tab5-ip>` in any browser for a full-featured live panadapter with mouse-driven tuning, band/mode controls, and ADIF log download. The IP is shown in the bottom status bar. See [Web UI](#web-ui).

### Step 6 — FT8 (if that's your thing)

Swipe in from the **left edge** to switch to the FT8 screen. The Tab5 starts decoding 15-second slots immediately — no PC, no WSJT-X required.

1. Tap the **Preset** button and pick your band's conventional FT8 dial frequency (e.g. 14.074 MHz for 20 m).
2. Watch the decode list fill. CQ stations appear at the top sorted by SNR; exchanges and replies below.
3. To reply to a station: **hold your finger on their row** for ~250 ms. A dim highlight appears after ~80 ms so you can see which row you're targeting before the gate fires. Lift — a confirmation modal shows the exact message before anything is armed.
4. Tap **Auto Pounce** to hand the full QSO to the auto-engine (works through report → RR73 → 73 with patient retry), or **Transmit** for a single manual message.
5. To call CQ: tap **Call CQ**. The engine picks a clear audio slot, fires CQ, automatically answers the first caller, runs the full exchange, logs the QSO, and resumes calling CQ.

Every completed QSO is written to an ADIF log downloadable from the web UI. See [FT8 Transmit](#ft8-transmit) for the full picture, including ARRL Field Day exchange mode and a no-radio-keyed practice/simulation mode.

### Step 7 — Something not working?

**Spectrum flat, top bar shows `---`:**
- Check the cable between Tab5 and QMX — almost always a charge-only cable.
- Make sure the QMX is powered on and not stuck in its own bootloader.
- Power cycle in order: Tab5 first, then QMX.

**For anything else:**

1. The diagnostic log is **always on** — nothing to enable.
2. Reproduce the problem (let it run a minute; power-cycle the QMX if the issue is about connection).
3. Grab the log:
   - **Over WiFi:** browse to `http://<tab5-ip>/api/log` or click **Diag ↓** in the web UI bottom bar — downloads `qmx-log.txt`. After a reboot/power-loss, **Diag(saved) ↓** (`/api/log/saved`) has the copy persisted to flash from before the reboot.
   - **microSD:** if a card is inserted, the log is mirrored to `/qmx-panadapter/qmx-log.txt` on the card — continuously with WiFi off, or up to the start-up backup with WiFi on (alongside the always-on flash-persisted copy, which is complete either way).
   - **Over USB (no WiFi needed):** capture the serial console with `tools/capture_serial_log.ps1`.
4. Open an [issue](https://github.com/SteffenLav/qmx-panadapter/issues) and attach the log. It includes Tab5 and QMX firmware versions plus every CAT command exchanged — usually enough to pinpoint the problem immediately.

---

## Getting help

**This whole guide is inside the firmware.** No WiFi, no microSD card, no download: it works on the first boot, in a field with no signal, and it can never describe a different version than the one you are running. Swipe ← from the right edge to open the settings drawer; the top two buttons are the two ways in.

**User Manual — opens where you are (new in v1.5.0).** Not a contents page to search: it opens the chapter covering the screen you were on — the panadapter chapter, the FT8 **receive** chapter, or the FT8 **transmit** chapter if a transmission is armed or running (someone mid-transmission is asking a different question). Inside: **Contents** is a two-column list — press and slide, and lifting your finger opens the highlighted chapter; **Back** returns to the previous page (shown only when there is one); **Exit** puts you back where you came from. Edge swipes and top-bar taps are stood down while the manual is open, so a stray touch cannot retune behind it. Holding the **User Manual** button for 3 s resets the reader; the manual itself is part of the firmware and cannot be lost.

**Need guidance? — describe the symptom, not the cause (new in v1.5.0).** The second drawer button opens a short list headed *"What do you need help with?"*, written the way you would say it out loud — "My radio is not showing up", "Nothing appears in the decode list", "It never transmits", "How do I change what my CQ says?", "Where are my contacts logged?" Pick the one that fits and the manual opens at the section that answers it. The list holds questions as well as faults, and it scrolls — keep going past the first few rows.

**Rows the Tab5 can see are happening now are highlighted and floated to the top**: no CAT link to the QMX, IQ mode never confirmed, an empty decode list while FT8 is running with the radio present, or WiFi switched on but not connected. WiFi switched **off** deliberately — POTA, battery — is not reported as a fault; that row stays as a normal question. **The device ranks, you choose:** a highlighted row is a suggestion, and it will never navigate for you on inference. The rows offered are scoped to the screen you opened the panel from, so FT8 never offers you spectrum symptoms.

**Warnings you can tap (new in v1.5.0).** A warning you cannot act on is half a warning. The red **"QMX IQ mode not confirmed"** banner across the top of the screen is a button — tap it for the section on that exact fault. The **"Waiting for QMX"** message carries a small **Need help?** button; it says *help* rather than *what is wrong* on purpose, since a radio that is off is often off deliberately.

If a chapter does not answer your question, that is a documentation bug worth reporting — the wording of these rows is written from what operators actually say.

---

## Panadapter

### Spectrum and waterfall

The display is divided into a **60 px top bar**, a **200 px spectrum** (green curve with dim fill), a **32 px frequency axis**, a **370 px waterfall**, a **22 px band-plan strip**, and a **36 px bottom bar**. The full visible span is 48 kHz centred on the QMX VFO.

The **spectrum** shows signal power in dBm (default range −130 to −30 dBm). Each frame is smoothed per-bin with an exponential moving average (EMA α = 0.4 by default, adjustable in the drawer), balancing visual stability against snappy response to CW signals and SSB attack transients.

The **frequency axis** shows absolute MHz labels centred on the QMX VFO, refreshed on every CAT frequency update. At high zoom levels the labels resolve to kHz or Hz precision.

**Band-plan strip.** A thin coloured bar directly above the bottom status bar (below the waterfall) shows the coarse CW / Digi / Phone segments of the current band, with a marker for where the VFO sits within them. Pick which IARU region it reflects in the settings drawer → **Band-plan region** (Auto from your grid square, or a fixed Region 1 / 2 / 3).

The **waterfall** runs newest row at the top in a thermal SDR palette (black → dark blue → teal → green → yellow → red). Four colour maps are available in the settings drawer: Thermal, Viridis, Turbo, and Grayscale.

**Waterfall floor tracking.** The waterfall's black level tracks the band noise automatically — a running median sampled only from bins inside the passband, EMA-smoothed — so the background colour follows conditions rather than a fixed anchor. Bins outside the passband run darker and are excluded from the floor calculation so they don't wash out dim in-band signals.

**Flat-spectrum mode** (toggle in the settings drawer, persisted to NVS) switches both spectrum and waterfall to a per-bin adaptive display: each bin renders as dB above its own running noise floor. Real signals — including weak CW tones — stand out sharply against a calm baseline. This is the recommended mode on noisy bands. The dB range sliders have no effect in flat mode; the axis shows relative dB above floor.

### Touch-to-tune

Tap anywhere on the spectrum or waterfall to place the cyan tune cursor. Drag and the cursor snaps grid-point to grid-point — you can see exactly which frequency will be tuned before you lift. The snap grid is **mode-aware**:

**Tune vs. pan — how your finger is read.** A quick horizontal swipe (more than ~70 px within 250 ms) pans the view instead of tuning — see [Zoom and pan](#zoom-and-pan). A slower touch-and-hold (250 ms without that much movement) locks into tune mode, after which dragging moves the snap cursor as described below. In short: swipe fast to pan, hold-then-drag to tune.

| Mode | Snap step |
|------|-----------|
| CW / CW-R | 10 Hz |
| USB / LSB | 250 Hz |
| FT8 / DiGi / RTTY | 500 Hz |
| AM | 1 kHz |

The grid is anchored to absolute frequency (e.g. …200 / 300 / 400 Hz), not to your touch start point, so the cursor always lands on the same set of grid points regardless of where your finger first touches.

A floating frequency tooltip above the cursor shows the target frequency in real time while dragging. Lift → CAT `FA` command is sent; QMX retunes; spectrum re-centres.

Taps always tune to exactly where you touched (snapped to the mode-aware grid above). The old **Snap to signal** option — which hunted for the strongest bin near your tap — was removed in v0.19.4; predictable tuning won.

**Passband indicator.** Two grey vertical lines mark your current filter edges. A faint coloured tint fills the passband. The amber VFO marker shows where the QMX is tuned; in CW mode it sits at dial + CW pitch offset so it marks the actual received tone frequency, not the suppressed carrier.

### Zoom and pan

| Gesture | Effect |
|---------|--------|
| One-finger fast horizontal swipe | Pan/"stroll" the view — retunes to the new centre frequency on release |
| Pinch (two fingers) | Zoom ×1.0 – ×24.0 |
| Two-finger drag | Pan the zoomed window |
| Double-tap | Reset zoom and pan to ×1.0 / centred |
| Top-bar Zoom → tap | Zoom preset: ×1 / ×2 / ×4 / ×8 / ×16 / ×24 |

**One-finger pan (stroll).** A fast horizontal swipe — more than ~70 px of movement within the first 250 ms of touching down — slides the spectrum and waterfall under your finger in real time, with a live frequency tooltip, and retunes to wherever you release. This works at any zoom level alongside the two-finger pinch/pan above; it's the quickest way to slide along a band without lifting into a deliberate tune-drag.

At zoom > ×1 the view **centres on the passband** (not the VFO dial), which matters for USB/LSB where the passband sits offset from the carrier. The passband lines and frequency axis track correctly at all zoom levels. Zoom level is persisted to NVS and restores with full zoom-FFT resolution on the next boot.

### S-meter

The Signal field in the top bar is a visual tick-scale bar labelled S1, S3, S5, S7, S9, +10, +20 with a moving green bar beneath. It is driven by the peak dBm in a ±64-bin window centred on the IF-shifted VFO bin — the actual signal under your cursor, not the DC/LO artefact at bin 0.

S-unit mapping: S9 = −73 dBm; 6 dB per S-unit below S9; 1 dB per unit above S9. The meter stays live during FT8 capture.

### Memory channels

Swipe up from the bottom edge (or tap the bottom grip handle) to open the 4×8 memory channel grid (32 slots, NVS-persisted).

| Action | How |
|--------|-----|
| Recall | Tap a slot — QMX retunes to stored frequency and mode |
| Create | Tap an empty slot — the frequency/mode picker opens directly |
| Edit | Long-press an occupied slot |
| Move | Long-press and drag a filled slot onto an empty one — the data follows your finger |
| Delete | Drag a filled slot onto the **wastebin** (bottom-right cell, channel 32) — it fades out |

Memory slots show the label in large text and mode + frequency (dimmed) below. The frequency/mode picker pre-fills the current VFO and lets you edit both before naming.

A new device ships with a few **example channels** already filled (rather than 32 blank cells), seeded only into empty slots so they never overwrite anything you've saved. The **first time** you open the picker, a brief one-time (~10 s) tour demonstrates the drag-to-move and drag-to-wastebin gestures, then never plays again.

### Settings drawer

Open by swiping in from the right edge, or tapping the right grip handle. The drawer is scrollable.

Controls appear top to bottom in this order:

| Control | What it does |
|---------|--------------|
| **Flip 180°** | Inverts the whole display and touch axes for upside-down mounting; centred checkbox so it isn't hit by accident, persisted |
| **QMX volume** | The radio's own AF gain, **in decibels — the same number the QMX shows on its LCD** (verified side by side against a real QMX). Reads the rig back each time the drawer opens, so it stays in step if you use the radio's volume knob. Intended for QMX+ builds with no control panel, where there is no knob at all. Spans 0–50 dB, not the QMX's full 0–199 dB, so the useful travel isn't crammed into the first couple of centimetres — 50 dB is Randy N4OPI's with-antenna figure. Turn the rig's own knob past 50 and the slider knob pins at the end while the number keeps showing the true dB |
| **IQ Balance** | Toggle adaptive I/Q image correction; re-enabling resets the estimator |
| **Flat spectrum** | Toggle flat/absolute display mode, persisted |
| **Presets** | HF Normal / HF DX / Strong Sig — sets dB range and smoothing in one tap |
| **WiFi** | Opens credential modal; **WiFi initiated** checkbox enables/disables WiFi entirely |
| **Identity** | Callsign + Maidenhead grid (required for FT8/FT4 TX; also drives KM/BRG columns) |
| **Band-plan region** | Auto (from your grid square) / Region 1 (EU/AF) / Region 2 (Americas) / Region 3 (Asia/Pac) — drives the [band-plan strip](#spectrum-and-waterfall); sits right under Identity since Auto derives from your grid |
| **dB Range** | Min and Max sliders (dBm) |
| **Smoothing** | EMA alpha 0.05–1.00 |
| **CW** | CW sidetone centre, 600–800 Hz; touch-to-tune in CW mode snaps to this offset, persisted |
| **IF calibration** | ±100 Hz trim for per-unit LO variance (see [Per-unit IF calibration](#per-unit-if-calibration)) |
| **Display** | Brightness, 10–100%, persisted |
| **Battery charge limit** | Optionally stop charging at a set percentage (default 80%) to reduce long-term pack wear; charging resumes automatically if the level later drops well below it (5% hysteresis). The displayed charge % now also compensates for the voltage rise while charging, so it no longer jumps around when plugged in |
| **Waterfall colour map** | Thermal / Viridis / Turbo / Grayscale, persisted |
| **Waterfall** | Black level, Contrast, Adaptive floor blend, and FFT window — see [Waterfall colourisation](#waterfall-colourisation) below |

**Grouped, with a Basic/Expert toggle (v1.6.0).** The drawer had grown to twenty-five sections in the order they were built, so related controls were scattered — the two QMX gain controls sat together, but CW pitch was nine sections away from the CW transmit offset. It is now grouped under headings — **Station**, **Device**, **Radio**, **Network**, **Display**, **FT8**, **Spectrum** — and the toggle beside the **Settings** title chooses how much you see. It always says where you are and what a tap gives you: **BASIC (tap for Expert)** reveals the Spectrum and Device groups, which hold the calibration and tuning controls you set once and rarely revisit. Nothing is lost in Basic, only hidden, and the choice is not remembered between sessions — it is a way of looking at the drawer rather than a preference.

The **Radio** group is where the controls that reach the QMX live: **QMX volume**, **QMX RF gain** (per band, read back from the radio), the **CW transmit offset**, **Antenna Tune** on 1.04+ firmware, and **Release radio to QMX menu**.

Earlier declutter, still true: the **Snap to signal** and **FT8 sync lines** toggles were removed in v0.19.4 (taps now always tune where you touch).

On the FT8 screen the spectrum-related groups drop away and three FT8-only controls appear: **Distance in miles** for the decode list's KM/MI column, **Fast pounce (early decode)** — see below — and **FT8 Simulation Mode**, see [FT8 Simulation mode](#ft8-simulation-mode).

**Fast pounce (early decode)** — on by default. Decodes surface ~1.8 s *before* the slot boundary (the way WSJT-X decodes in the dead-air gap), so replying to a fresh CQ can transmit in the very next slot instead of waiting a full 30 s cycle, and mid-QSO replies land on the beat. The trade-off: the capture window closes ~1.8 s early, so a station transmitting *late* in the slot can occasionally be clipped and missed. ⚠️ This feature has not yet been A/B-verified on a live band — if your decodes-per-slot drop noticeably with it on, turn it off here and please report your before/after numbers on the groups.io thread.

### Waterfall colourisation

Four live, NVS-persisted sliders/dropdown at the bottom of the settings drawer fine-tune how the waterfall maps signal to colour — changes scroll in from the top as you drag:

| Control | Range | Default | Effect |
|---------|-------|---------|--------|
| **Black level** | 0–30 dB | 9 dB | How far above each bin's own noise floor a signal needs to be before it lifts off black |
| **Contrast** | 10–80 dB | 45 dB | The dB span that fills the rest of the colour ramp above the black level |
| **Adaptive floor** | 0–100% | 100% | Blend between a per-bin noise floor (100%) and one global mean floor across the band (0%) |
| **FFT window** | Blackman-Harris / Hann (sharp) / Nuttall | Blackman-Harris | The FFT window function; Hann trades some sidelobe suppression for sharper peaks |

---

## Web UI

With the Tab5 on WiFi, open `http://<tab5-ip>` in any modern browser. The IP is shown in the bottom status bar on the Tab5.

The browser panadapter is a full-featured view in its own right — not just a window onto the Tab5. On a larger monitor you get more spectrum history, a bigger waterfall canvas, and mouse controls that are faster than touch for precise tuning. It shows live spectrum at ≈10 fps via WebSocket, full waterfall history (~50 s), the same thermal palette and floor maths, a graphical S-meter, and a top bar with Band / Mode / BW / Zoom controls. The bottom bar shows battery percentage + voltage, firmware version, a live UTC clock, and WiFi SSID + RSSI. To its right: download/upload buttons (ADIF, QRZ, eQSL — see [QSO logging](#qso-logging-adif)), **Diag ↓** for the diagnostic log, **Config ↓ / Config ↑** to back up / restore / edit all settings (see [Config backup, restore & edit](#config-backup-restore--edit)), and **Tab5Shot** which opens a live `/ss.bmp` screenshot in a new tab.

**microSD file browser (new in v1.3.0).** **Files → SD Files** in the bottom bar opens `http://<tab5-ip>/files` — browse the microSD card from any computer without pulling it: download your logs, config backups, and offline manual; upload files; delete. Card access is coordinated with the WiFi link the same way the automatic backup is, so it's safe to use mid-session.

**Whole-band plan strip & adjustable split (new in v0.20.0).** Along the bottom of the browser view (above the status bar), a colour-coded CW/Digi/Phone strip spans the entire band with a draggable "visible window" (drag or tap to retune) and a VFO marker, mirroring the Tab5's own strip. Drag the divider between the spectrum and the waterfall to give either more room — the split is remembered in the browser. **Tab5Shot** now captures any open pop-up (band/mode dropdown) too, and the frequency keypad is draggable with a standard 10-key layout.

**In FT8/FT4 mode the live stream pauses (new in v0.20.0).** The browser stops streaming the spectrum/waterfall and shows a notice plus the log and upload controls instead. While you're operating digital modes the stream would compete with the on-device decoder and the WiFi link, so pausing it keeps FT8 decoding and WiFi noticeably steadier. Switch the Tab5 back to Panadapter mode and the stream resumes automatically.

**Live TX status, and Call CQ from the browser.** In FT8/FT4 mode that same panel carries a **TX status banner** (v1.3.6) mirroring the Tab5's own label — red while transmitting including the "call 2 of 4" counter, amber armed, green on QSO complete, orange on timeout, plus the persistent "CQ stopped after N calls - no answer"; the browser tab title shows a red dot while transmitting, visible even in a background tab. Under it is a **Call CQ** button (v1.5.0, Dennis WN4FLA), so a CQ run that timed out or reached its call limit can be restarted without walking back to the radio. It **confirms first** — it keys the radio, and a mis-click from another room should not put a carrier on the air — then uses exactly what the Tab5 would: the active CQ preset, the current TX tone (honouring TX Hold) and the TXCQ ANY/EVEN/ODD parity, sharing one code path with the Tab5's own button so the two cannot drift. The request is handed to the display task, so the button greys out for a moment; watch the banner rather than the button. A request arriving while the Tab5 is not in FT8 is discarded, never queued. Replies and pounces are still initiated on the Tab5, where the decode list is.

**Click or drag to tune.** Click or drag on the spectrum or waterfall — a cyan cursor appears with a live frequency readout and commits on release.

**Mouse-wheel to pan or tune.** Rolling the mouse wheel over the spectrum or waterfall **pans** the view when zoomed in, letting you survey the band without touching the Tab5. At zoom ×1 the wheel **tunes** with mode-aware snap (CW 10 Hz, SSB 500 Hz, DiGi 100 Hz, AM 1 kHz).

**Band / Mode / BW / Zoom** dropdown pills in the top bar send commands to the QMX via `/api/cmd` — the same effect as using the Tab5 top bar.

**Zoom sync.** The browser renders the same zoomed window as the Tab5.

**QSO log — download, view, edit.** Once you have logged at least one completed FT8 QSO, a **"QSO Logs (N) ▲"** menu appears in the bottom bar of the web UI. **ADIF download ↓** fetches your `qso.adi` file directly. **View / edit log** (v1.3.5) opens the whole log as a table in the browser — click any column header to sort (click again to reverse; sorting by date groups an activation's QSOs together), delete a single record with the ✕ on its row, or **Delete all** to clear the log (it asks you to type `DELETE`, because there is no undo — download the ADIF first if you want a copy). The menu is only shown when the log contains data.

**QRZ / eQSL upload.** Two more buttons appear alongside the ADIF link once you have logged QSOs — see [QSO logging](#qso-logging-adif) for the full picture.

### Config backup, restore & edit

The **Config ↓ / Config ↑** buttons in the bottom bar let you save every setting to a plain text file, edit it on your PC, restore it, or share parts of it.

- **Config ↓** downloads `qmx-config.txt` — a human-readable [INI-style](https://en.wikipedia.org/wiki/INI_file) file with everything the Tab5 remembers, grouped into sections:
  - `[settings]` — callsign, grid, WiFi SSID/password, CW pitch, IF trim, IQ balance, flat-spectrum, zoom, colour map, brightness, dB range, EMA, GPS toggle, keypad layout, QRZ key, eQSL user/pass.
  - `[cq]` — the three FT8 CQ-message presets and which is active.
  - `[ft8_filters]` — the CQ-run include/exclude filter terms and toggles.
  - `[memories]` — the 32 memory channels, one per line: `slot = freq_hz, mode, label`.
- **Config ↑** picks an edited file and **merges** it back: only the keys and sections present in the file are changed — everything else is left as-is. Unknown keys are ignored (so a file from a newer firmware still loads). Memory channels apply immediately; other settings take effect after a restart.

Three things you can do with it:

1. **Back up before a clean flash.** Hit **Config ↓** first, do the clean/erase flash, then **Config ↑** to restore everything in seconds — instead of re-typing it all on glass.
2. **Edit in a text editor instead of on the touchscreen.** Faster for callsign, grid, CQ messages, or punching in a batch of memory channels.
3. **Share a band plan.** Copy just the `[memories]` section into a message — another operator pastes it into their file and uploads it to get the same channels (their other settings untouched).

> The downloaded file contains your **WiFi password** and **QRZ/eQSL credentials in clear text** (so it works as a full backup). Keep it private; strip those lines before sharing a file with anyone.

### Endpoints

| Endpoint | Method | Returns |
|----------|--------|---------|
| `/` | GET | Browser panadapter (HTML) |
| `/api/status` | GET | JSON status (see below) |
| `/api/cmd` | POST | Send Band/Mode/BW/Zoom commands |
| `/api/log` | GET | Diagnostic log download (`qmx-log.txt`) |
| `/api/log/saved` | GET | Flash-persisted diagnostic log from before the last reboot/power-off |
| `/api/adif` | GET | ADIF QSO log download (`qso.adi`) |
| `/api/adif/clear` | POST | Wipe ADIF log and worked-call cache; resets the QRZ/eQSL/LoTW upload positions |
| `/api/adif/delete` | POST | Delete one record: `?idx=<n>&call=<CALL>` — both must match the record or it answers 409 |
| `/api/qrz_key` | POST | Save QRZ Logbook API key (body = raw key text) |
| `/api/qrz_upload` | POST | Upload pending QSOs to QRZ Logbook; returns `{uploaded, failed, error}` |
| `/api/eqsl_creds` | POST | Save eQSL username/password (JSON body `{"user","pswd"}`) |
| `/api/eqsl_upload` | POST | Upload pending QSOs to eQSL (batched); returns `{uploaded, failed, error}` |
| `/api/config` | GET | Download all settings + memory channels as editable INI (`qmx-config.txt`) |
| `/api/config` | POST | Upload an INI config; merges into NVS; returns `{applied}` |
| `/ss.bmp` | GET | 1280×720 RGB565 BMP screenshot |
| `/ws` | WS | Binary spectrum stream (~10 fps) |

### `/api/status`

```json
{
  "battery":     { "level": 100, "mv": 8320, "charging": true },
  "wifi":        { "ssid": "MyNet", "rssi": -44, "ip": "192.168.1.213" },
  "freq_hz":     14074000,
  "mode":        "USB",
  "band":        "20m",
  "passband_hz": 2700,
  "signal_dbm":  -87.4,
  "zoom":        1.0,
  "pan_bins":    0,
  "flat_mode":   false,
  "utc_epoch":   1750000000,
  "qso_count":   12,
  "qrz_key_set": false,
  "eqsl_creds_set": false,
  "tab5_fw":     "v0.18.8",
  "qmx_fw":      "1_03_002QMX"
}
```

Polled at 1 Hz by the landing page; safe to consume from monitoring scripts, home automation, etc. `qmx_fw` is read from the QMX via the `VN;` CAT command at link-up (empty until the radio responds). `signal_dbm` is the peak dBm around the IF-shifted VFO bin (null if DSP has no data yet). `qrz_key_set` / `eqsl_creds_set` tell the web UI whether to prompt for credentials before the next upload.

### `/ws` — binary spectrum WebSocket

Single-client endpoint. Each binary frame is 1026 bytes:

| Bytes | Meaning |
|-------|---------|
| `0` | Frame type: `0x01` = spectrum |
| `1` | Zoom decimation factor (1 = base 1024-bin spectrum, >1 = zoom-FFT with that decimation) |
| `2–1025` | 1024 unsigned bytes, one per bin, quantised −130 dBm (0) to −30 dBm (255) |

Bins are pre-shifted server-side so byte 2 is the leftmost screen pixel. Push rate ≈10 fps.

### Screenshots

```powershell
Invoke-WebRequest -Uri "http://192.168.1.213/ss.bmp" -OutFile screenshot.bmp
```

---

## FT8 Receive

![FT8 RX in action on 20 m — decode list with DXCC, distance, bearing, and SNR](docs/QMX-Panadapter_FT8_v0.15.7.png)

*Live FT8 reception on 20 m (v0.15.7). Left pane: MODE / Preset freq / UTC / slot countdown with parity and progress bar / TX parity preference / operator identity / Call CQ / decode summary. Right pane: scrollable decode list.*

Swipe in from the **left edge** to switch to the FT8 screen. The Tab5 decodes 15-second FT8 slots directly on the ESP32-P4 — no PC, no WSJT-X.

### Prerequisites

- **WiFi configured** — recommended for accurate UTC time. Without it the Tab5 falls back to its supercap RTC or the QMX clock. See [Time sync](#time-sync).
- **Callsign and grid** set in the drawer → **Identity** if you want distance/bearing columns or plan to transmit.

### The FT8 screen

**Left pane:** MODE label · **Preset** frequency button (tap to pick a band's conventional FT8 dial frequency) · UTC clock · 15-second slot countdown with current parity (EVEN blue / ODD amber) and a gliding progress bar · **live mini occupancy strip** (the audio window as 50 Hz cells, same data and colours as the tone picker) · **TXCQ ANY / EVEN / ODD** parity-preference button (one button, cycles) · **TX nnnn Hz** tone button · **Call CQ** button · last-slot decode summary and TX status.

**Right pane — decode list:**

| Column | Content |
|--------|---------|
| SL | Slot parity: **E** (blue, :00/:30) or **O** (amber, :15/:45) |
| CALL | Extracted callsign |
| MESSAGE | Full decoded FT8 message text |
| CTY | DXCC entity as a 3-letter code (ISO alpha-3 where it exists; ~190 entities) |
| SNR | FFT-based estimate, colour-banded: green ≥0 / white −5..−1 / orange −15..−6 / grey <−15 |
| DT | Slot-timing offset in seconds, relative to the band — an on-time station reads ~0.0 |
| HZ | The station's audio tone (its offset within the FT8 passband) |
| KM | Great-circle distance from your grid |
| BRG | Bearing from your grid |
| HRD | Times decoded since last appearance |

CQ calls always appear at the top sorted strongest-SNR first; all other rows follow by SNR descending.

**Row colour scheme** (CALL + MESSAGE columns): **white** for ordinary traffic, **green** for plain `CQ` calls, **dim grey** for callsigns already worked **on the current band** (a station you worked on 20 m still shows normally on 40 m — see [Reply filter](#reply-filter) for hiding them entirely), and **inverted red fill + white text** for any message containing your own callsign — your highest-priority rows literally pop off the screen instead of relying on red-on-black text, which field testing found hard to read at a glance.

**Live view.** Stations not re-decoded within 60 seconds drop off the list automatically, even while the band is quiet — the list is who's on frequency *now*, not a cumulative history. (The "Active: N" counter that used to say so was dropped in v1.3.4; the wrapping TX/QSO status text took its line.)

### Performance

On a busy 20 m FT8 slot the decoder regularly yields 25–50 callsigns per slot. Both EVEN and ODD slots decode every cycle via a ping-pong dual-buffer architecture — a TX slot never causes the opposite parity to be skipped. Heap is stable across long sessions (≈39 KB internal RAM free, 25 MB PSRAM free during decoding).

---

## FT8 Transmit

The Tab5 transmits FT8 via the QMX's `TA<freq>;` CAT command — no PC audio path, no WSJT-X. The QMX does all DDS synthesis and envelope shaping; the Tab5 sends the 79 tone-frequency commands at 160 ms cadence, bracketed by `TX;` / `TA0;` / `RX;`.

### Replying to a station

1. In the decode list, **hold your finger on a row** for ~250 ms. A dim highlight appears after ~80 ms so you can see which row you're targeting. List scroll locks once the gate fires; drag up or down to land on the right row.
3. **Lift** — a confirmation modal shows the exact message that will go on air, the audio frequency, and the target slot parity.
4. Tap **Auto Pounce** to hand the full QSO to the auto-engine, or **Transmit** for a single manual message.
5. Tap **Cancel** (in the modal, or the armed indicator in the left pane) to disarm without transmitting.

**Transmit is intelligent (v1.3.0):** it builds the correct *next* message for that station from what they last sent — the same semantics as a WSJT-X double-click. Their CQ → your grid (or your report directly, if **Skip TX1** is on); their grid → your report; their report → `R`+your report; their `R`-report → `RR73`; their `RR73`/`73` → `73`. The report value is always your live measurement of their signal. You can walk an entire QSO step by step with nothing but Transmit taps — and when you send the closing `RR73`/`73`, the QSO **logs to ADIF** exactly like an auto-engine contact. Auto Pounce is offered on any first reply; mid-QSO rows get Transmit only.

Slot parity is set automatically — if you heard them on an EVEN slot, your reply goes on ODD so they're listening when you transmit.

**The auto-engine** works the full exchange: TX1 (grid) → wait for their report → TX2 (R+report) → wait for RR73/73 → TX3 (73) → DONE. At every step it re-sends the current message for up to 4 consecutive slots if the other station doesn't respond. If no reply comes after 4 slots, the QSO times out (orange status, tap to clear).

**Skip TX1 (faster pounce).** With **Skip TX1** enabled in the Filter editor, a pounce opens with your signal report immediately instead of the grid exchange — saving one round trip. If the station has already aged out of the decode list, it falls back to the normal grid TX1 automatically.

### Working a pile-up

When you call CQ or work a run, more than one station may answer at once — and a caller who replies while you're mid-QSO with someone else used to vanish from the live decode list once they stopped transmitting. They're now held in a **Pileup** list so you don't lose them:

- Whenever callers are waiting, the **ADIF-log** button on the FT8 screen becomes a **Pileup** button in a distinct colour, reverting once the list empties.
- Tap **Pileup** to see everyone who has called you and isn't worked yet. Tap a station to work them (the same confirmation modal as a decode-list tap), or tap the **✕** to dismiss one.
- **Hold the button to open the ADIF log** — the log is always reachable this way, even while the button reads "Pileup" (v1.3.0; a one-time hint appears the first time the button flips).
- A station is removed from the list automatically once you start a QSO with them, and a just-completed contact's trailing 73 can't put them back.
- Working a caller from the pileup sends the correct *next* message for that station, built from whatever they actually last transmitted — the same laddering as tapping a decode-list row (v1.3.2). Previously it always opened with a signal report, which could never produce the `R`+report a station needs when they come back minutes later with a report of their own.
- Worked-before stations appear in the pileup unless **Exclude worked-before** is checked — the pileup follows the same rule as the auto-answer, so dupes you're willing to work stay visible.

The tracker never transmits on its own — it only remembers callers; you choose who to work. If you'd rather it *did* transmit, check **Auto-work pileup** in the Filter editor: when your current QSO completes (or immediately, if you check it with callers already waiting and nothing else going on), the strongest waiting caller is pounced automatically, draining the pile one contact at a time. It carries the same unattended-TX warning as the robot.

### Calling CQ — CQ-run mode

Tap **Call CQ**. No confirmation modal. The engine:
1. Scans the current decode list for occupied 50 Hz audio bins.
2. Picks the nearest unoccupied bin to 1500 Hz (standard FT8 sub-band centre).
3. Arms the CQ on the matching slot parity and waits for the boundary.

From there it's fully automatic:
- The opposite-parity slot is decoded while CQ runs so replies are never missed.
- The moment a station replies with your callsign, CQing stops and the exchange starts automatically — best-SNR caller chosen if multiple stations answer simultaneously.
- After the exchange completes (RR73 → 73 → logged), CQ resumes on the same frequency.
- A mid-exchange timeout also resumes CQ rather than dropping the frequency.
- While CQ-run is active, other stations' `CQ` rows are hidden so replies to you stand out.

Tap **Cancel** in the TX status bar at any time to stop.

### Reply filter

Tap **Filter** in the left pane to open the filter editor. Controls which stations CQ-run auto-answers and which rows appear in the decode list.

- **Include 1 / Include 2** — if either field has text, only messages containing one of its terms are eligible. Space- or comma-separated (e.g. `POTA SOTA` or `JA, VK`), matched against the *whole* decoded message text so POTA/SOTA tags, grid squares, country prefixes, `/P` suffixes etc. all work.
- **Exclude 1 / Exclude 2** — messages containing any of these terms are skipped even if they'd otherwise match.
- **Exclude plain CQ callers** — hides bare `CQ ...` rows so only replies and exchanges remain visible.
- **Exclude worked-before** — hides callsigns already worked **on the current band** (per-band, from your ADIF log) from the list and from CQ-run auto-replies; the same station on a different band is treated as not worked.
- **Skip TX1** — when you pounce a station, open with your signal report instead of the grid exchange for a quicker QSO (falls back to the grid exchange if the station has aged out of the decode list).
- **Allow grey-listing** — off by default. When enabled, a station that times out two of your pounces in a row is set aside: the robot and Auto-work-pileup skip it, its decode row turns violet, and tapping it offers to clear it rather than opening the TX dialog. Useful on a busy band where one station simply never comes back. The list lives in RAM only and is forgotten at power-off.

Tap **Save** to persist (NVS) and apply immediately.

### Auto-answer CQ (robot)

**⚠ Transmits unattended — never leave it running unsupervised.** When enabled, the robot picks a CQ caller every slot (filtered the same way as above, plus a worked-before skip) and runs the full exchange itself — no per-QSO confirmation, no tap required. Pick a **Priority**: Strongest signal, Weakest signal, or Most distant grid. Same TX1→report→RR73→73→ADIF-log flow as a manually-tapped reply; you just aren't the one tapping. You remain responsible for everything it transmits under your callsign, same as any other unattended digital-mode software.

### ARRL Field Day mode

A checkbox in the same Filter modal as above (with Class/Section text fields next to it) switches the FT8 exchange from grid/signal-report to ARRL Field Day's class+section format — e.g. `16A EMA` instead of a grid square, using the standard `WA9XYZ KA1ABC R 16A EMA`-style FT8 message type (the same one WSJT-X uses for FD). Pounce and CQ-run both follow the convention automatically: the initial grid-exchange message is unchanged, but the report-equivalent step carries class+section instead, with the receiving side echoing it back `R`-prefixed.

While the mode is on, **Call CQ** automatically tags your CQ message `CQ FD <call> <grid>` (the same "CQ modifier" mechanism as `CQ POTA`/`CQ DX`) so other Field Day stations know to expect this exchange instead of a normal report — this overrides any other modifier on the active CQ preset for as long as the mode is enabled. The long-press CQ preset editor reflects this: while Field Day mode is on, the three presets are shown dimmed (their own modifier doesn't matter right now) and a live preview line shows exactly what will be transmitted.

Completed Field Day QSOs log the standard ADIF contest fields (see the table below) alongside the usual call/freq/time fields, so they import cleanly into contest-logging software.

### FT8 Simulation mode

A **"FT8 Simulation Mode"** checkbox in the FT8 settings drawer (FT8 screen only) lets you practice everything — manual step-by-step Transmit, Auto Pounce, CQ-runs, pileups, and Field Day exchanges — with **no real station, no antenna, and no QMX connected at all** (v1.3.0; previously the radio had to be attached even though it was never keyed).

**Six phantom stations** (three US, three DX) call CQ on their own tones. Each phantom message is a real FT8 message, synthesized to actual GFSK audio and decoded through the same on-device receive pipeline real RF goes through — what lands in the decode list genuinely round-tripped the receiver. The phantoms behave like real operators:

- Tap a CQ to pounce (auto or fully manual — they answer either), or **Call CQ** yourself and **four of them answer at once**, building a genuine pileup to practice the pileup tools on.
- They're **patient**: each message repeats every cycle, up to four times, until you respond — then they give up and go back to CQing. A phantom you're mid-QSO with stops CQing; one you've worked stops answering your CQs for the session (toggle sim off/on to reset).
- Their replies match **what you actually transmitted** — grid gets a report back, a report gets a roger, `RR73` gets a courtesy `73`.
- The **Fast pounce** toggle (below) is honoured: with it ON, phantom messages surface just before the slot boundary; with it OFF, just after — so you can see exactly what the toggle changes.
- Swiping to the Panadapter and back clears the phantom rows and pileup for a fresh session.

While simulation mode is on, a **breathing red border** frames the whole screen as an unmissable reminder that nothing transmitted right now is real — the hard interlock lives in firmware (every CAT command that would key the radio is skipped, logged instead), not just in the UI. Completed simulated QSOs log to the same ADIF file as real ones — deliberately, so the logging/upload paths get exercised too. When you're done practicing, the ADIF viewer shows a **"Del N test"** button (only while simulation records exist — they're recognizable by their missing frequency): two taps wipes every practice contact from the log.

### TX status indicator

The left pane shows a persistent status line below the slot countdown:

| Colour | Meaning |
|--------|---------|
| **Red** | `TRANSMITTING: <message>` — burst in progress, with your **audio tone on its own line**; tap to abort |
| **Amber** | `TX armed: <message>` then `nnnn Hz → EVEN/ODD, ~Ns` — waiting for slot; tap to cancel |
| **Red-orange ⚠ FREQ BUSY** | Your tone is occupied (±50 Hz guard), and the warning names the frequency so you can act on it. During CQ-run this self-heals — the next cycle hops to the nearest clear tone automatically. Mid-exchange you can now move it yourself: see **TX audio frequency** below |
| **Amber, `working <call> - waiting` + `TAP TO CANCEL`** | The station you are calling is mid-exchange with somebody else. Nothing is transmitted until they sign off or call CQ again — or tap to cancel the pounce and work someone else (v1.3.5). See **Waiting for a busy station** below |
| **Green** | QSO complete |
| **Orange** | QSO timed out — tap to clear |
| **Dim white** | FT8 engine status passthrough (capturing / decoding / symbol count / …) |

### TX audio frequency

Your transmit tone is chosen automatically — the nearest clear 50 Hz slot to 1500 Hz, scanned against the stations currently decoded. From v1.3.3 you can both **see** it and **change** it.

The **TX nnnn Hz** button in the FT8 left pane always shows it — to the right of the `TXCQ` parity button — and tapping it opens the picker. A **mini occupancy strip** under the slot countdown shows the same picture at a glance. (Before v1.3.4 this was a chip that appeared only while a CQ or QSO was running, and the tone was also repeated on the TX status line.)

| Control | What it does |
|---------|--------------|
| **Occupancy strip** | The whole 200-2800 Hz window as 52 slots. **Green** free, **red** occupied, **white** you, **pink** your QSO partner |
| **Touch and drag** | Pick a slot by dragging along the strip. The bar goes grey and follows your finger, the readout tracks it live, and it commits when you lift off |
| **-50 / +50** | Nudge one slot at a time |
| **Find clear slot** | Scans outward from where you are and jumps to the nearest free slot |
| **Apply nnnn Hz** | Sends it to the radio |

The readout also says in words whether the slot you have picked is clear or occupied, and the free slots nearest you are listed as numbers underneath.

It applies **between** bursts. If a burst is on the air, Apply refuses and tells you to try again after it — the same rule WSJT-X operators are used to. Slot parity is untouched, so moving your tone mid-QSO does not disturb the exchange; your partner tracks the slot, not the frequency.

> **The strip shows decoded stations, not raw spectrum.** A station too weak to decode will not show as occupied, and an all-grey strip means nothing has been heard yet rather than "the band is clear".

### Waiting for a busy station

On a crowded band several stations answer the same CQ and the caller works one of them. If that is not you, the Tab5 no longer keeps calling: while their last decoded message is addressed to somebody else, **nothing is transmitted**. The status line shows `working <call> - waiting`. As soon as they send `73` or `RR73`, or call CQ again, it picks up where it left off.

**You are not committed to the wait** (v1.3.5, Roy KI0ER's report): the status line carries a **TAP TO CANCEL** line during the hold, and tapping it drops the pounce so you are free to work anyone else. The abandoned exchange stays resumable for a few minutes in case the station frees up and you want back in.

This also protects the grey-list. Giving up on a station used to count against it, so a popular station could end up permanently skipped by the robot and Auto-work-pileup for no reason other than being busy. A wait is not a failed attempt. The wait is capped at about six minutes so a station that vanishes mid-exchange still times out normally.

### QSO override buttons

During an active auto-QSO exchange (any state between first reply and final 73), three buttons appear in the left pane:

| Button | What it does |
|--------|--------------|
| **Re-send / &lt;field&gt;** (amber) | Re-arms the current outgoing message immediately. The label shows what will go out — e.g. **Re-send / JO45** when waiting for their report, **Re-send / -07** when waiting for RR73 |
| **RR73** (blue) | Skips straight to RR73, bypassing the report step |
| **73** (green) | Fires the 73 sign-off and closes the QSO immediately |

These are deliberate operator nudges for busy-band edge cases where the auto-engine falls a slot behind or you can see the exchange is further along than the state machine knows. The auto-engine handles the normal case; these exist for when a quick nudge is faster than waiting through the 4-slot retry cycle.

### TX power / SWR readout

After each burst the Tab5 queries `PC;`/`SW;` for instantaneous forward power and SWR and shows the result briefly in the status line: `Last TX: X.XW SWRx.xx [Ns]`. If SWR > 4.0 (indicating the QMX SWR-protection latch tripped), the firmware automatically cycles `TX;` / 150 ms / `RX;` to clear the latch so the radio is ready for the next TX slot without operator intervention.

### CQ message presets

Long-press the **Call CQ** button to open the preset editor. Three message slots with radio buttons — check the active one. A `+ <call> <grid>` quick-insert appends your identity. Standard CQ constructions (`CQ DX OZ1LAV JO65FR`, `CQ POTA …`) and ≤13-char free text both encode via the general ft8_lib encoder. Presets persist to NVS. The Call CQ button label shows the selected message; a short tap transmits it.

**CQ auto-stop** (v1.3.5, Don WB0LQW's "I usually send CQ 2-4 times and then pause"). The editor's top-right **CQ stop** button cycles never / 1 / 2 / 3 / 4 / 5 / 10 calls and applies the moment you tap it — no Save needed. While calling, the TX status shows the counter live ("call 2 of 4"). After the last unanswered call the Tab5 listens through one more receive slot — an answer to your final call still starts the QSO normally — then stops and goes idle. The limit applies to every CQ run, including the automatic resume after a completed or timed-out contact; each fresh sequence starts the count over. Persists across power cycles and travels in the config backup.

### QSO logging (ADIF)

Every completed FT8 QSO — pounce or CQ-run — is automatically written to an ADIF v3.1.4 file at `/spiffs/qso.adi` on the Tab5's internal flash. The log survives reboots and re-flashes.

**Download.** The web UI bottom bar shows the **QSO Logs** menu once the first QSO is logged. **ADIF download ↓** fetches `qso.adi` for import into any logging software (WSJT-X, Log4OM, DXKeeper, …) or for POTA.app, which has no upload API (see below); **View / edit log** opens it as a sortable table with per-record delete and Delete-all — see [Web UI](#web-ui).

**View and edit on the Tab5.** The **ADIF Log** button on the FT8 screen (long-press always works, even while it reads "Pileup") opens the on-device viewer: a Today/All filter with a POTA activation counter (the title turns green at 10 QSOs today), long-press a row to delete that single record, and — new in v1.3.5 — **Delete all**, next to Close: the first tap arms it and shows the live count ("ALL 34?"), a second tap within five seconds erases the log, waiting disarms it. Clearing the log (from either the Tab5 or the browser) also resets the QRZ/eQSL/LoTW upload positions, so QSOs logged afterwards upload normally. The clear-first workflow is the practical POTA tool: start the activation with an empty log and the ADIF at the end is exactly what you submit.

**Upload to QRZ Logbook / eQSL — directly from the Tab5.** Two more buttons appear next to the ADIF download link once you've logged a QSO: **"QRZ ↑"** and **"eQSL ↑"**. Tap either the first time and it prompts for credentials (QRZ: API key from *Logbook Data → API Key* on qrz.com; eQSL: your username and password — eQSL has no API-key scheme) and saves them on the Tab5, not just in the browser. Every tap after that uploads everything logged since the last successful upload — no need to re-enter credentials, and no risk of duplicate uploads, since the Tab5 tracks how far it's gotten into the log. If a record is rejected (bad credentials, quota, etc.) the run stops and reports the reason rather than skipping past it; fix the cause and tap again to resume from where it left off. eQSL accepts a whole batch of QSOs per request; QRZ's API takes one at a time, so a big log takes one HTTP round trip per QSO.

**Upload to ARRL LoTW — also directly from the Tab5.** The Tab5 signs your QSOs on-device with your own LoTW callsign certificate, so no TQSL and no PC are involved. The bottom bar shows **"LoTW setup"** until it is configured, then **"LoTW ↑"**.

Setup is a two-page guided flow. Page one points you at ARRL's own "Save the Callsign Certificate" instructions — you export a `.p12` file from TQSL, which most people did once, years ago. Page two takes that file, its password, and your DXCC entity number:

| Field | Notes |
|-------|-------|
| **.p12 file + password** | Parsed **in your browser**, not on the Tab5 — the password never reaches the device. Only the certificate and key are sent |
| **DXCC entity number** | Required. The same one your TQSL station location uses (Denmark = 221, USA = 291) |
| **CQ zone / ITU zone** | Optional |
| **US state / county** | **US stations only** (v1.3.3). Without them your QSOs earn no Worked All States and no county credit — for you or for the stations you work. The county is the name on its own (`Arlington`), not `VA,Arlington` |

Every tap of **LoTW ↑** afterwards signs and uploads everything logged since the last successful upload. Ctrl-click **LoTW ↑** to re-run setup — a certificate is typically valid three years. Re-importing the *same* certificate no longer re-uploads your whole log; only an actually-changed certificate resets the upload position, since a new key means everything has to be re-signed.

> Uploads are queued at ARRL's end. If a QSO does not appear immediately, check [the LoTW queue status](https://www.arrl.org/logbook-queue-status) before assuming a problem — the queue has run hours behind at busy times. LoTW rejects a malformed file at upload time, so anything that reaches the queue was signed correctly.

**POTA.app** has no upload API at all (browser login or email only), so use the ADIF download above for that.

**Fields in each record:**

| ADIF field | Content |
|------------|---------|
| CALL | Their callsign |
| FREQ | Dial frequency in MHz (3 d.p., e.g. `14.074`) |
| BAND | Amateur band (e.g. `20M`) |
| MODE | `FT8` |
| SUBMODE | `FT8` (required by LoTW TQSL and eQSL for digital mode credit) |
| RST_SENT | Our SNR estimate (e.g. `−07`). **Omitted entirely if no report was exchanged** — writing a placeholder `599` into an FT8 log, as versions before v1.3.4 did, fabricates a measurement that then gets uploaded to QRZ/eQSL/LoTW |
| RST_RCVD | Signal report received |
| QSO_DATE | UTC date `YYYYMMDD` |
| TIME_ON | UTC time `HHMMSS` |
| MY_CALL | Your callsign (from Identity in the drawer) |
| MY_GRIDSQUARE | Your grid |
| GRIDSQUARE | Their grid (from the decoded FT8 message) |
| CONTEST_ID, STX_STRING, SRX_STRING, ARRL_SECT, MY_ARRL_SECT | Field Day mode only: contest ID `ARRL-FD`, your/their literal `<class> <section>` exchange text, and their/your section alone |

**Clear.** `GET /api/adif/clear` from the web UI wipes the file and resets the worked-call cache.

> Set your callsign and grid in the settings drawer → **Identity** before your first QSO so they appear correctly in logged records.

---

## Time sync

The Tab5 needs accurate UTC for FT8 slot timing. Sources in priority order (highest first):

| Source | When applied |
|--------|-------------|
| **SNTP (WiFi)** | Always authoritative whenever WiFi is connected and has synced at least once — sets system clock, Tab5 RTC, and NVS |
| **Tab5 RTC** | RX8130CE supercap-backed (~30–40 h retention) — applied at boot before QMX or WiFi are available |
| **QMX `TM;`** | Offline fallback only — applied when WiFi is down or has never synced (field/POTA with no WiFi) |
| **FT8 signal timing** | Sub-second correction from the decoded signal population's average timing, auto-applied continuously every slot while FT8 is decoding (damped, so a noisy single slot can't yank the clock) — deliberately tracks who you're actually trying to work, not absolute GPS/NTP truth. A manual one-shot version is also available via the **Sync Time** modal (see below) |
| **Manual** | Full HH:MM override in the **Sync Time** modal — for rare POTA sessions with neither WiFi nor QMX clock |

Each accepted sync writes through to the RX8130CE so the clock persists across power-off. The Tab5 also polls `TM;` every 5 minutes in the background to catch QMX GPS lock events.

**For POTA / no-WiFi use:** uncheck **WiFi initiated** in the settings drawer. Time comes from the QMX on USB connect (or the supercap RTC if the Tab5 was recently synced). FT8 slot timing will be accurate from either source.

### FT8 time calibration modal

On the FT8 screen, tap **Filter** → **Sync Time** to open the time calibration modal. It shows three large boxes: **\[HH\] : \[MM\] : \[SS\]**.

- **HH / MM** — pre-filled from the current UTC clock. Tap either box to edit it with a numpad. Use this for rare POTA situations where neither WiFi nor the QMX clock is available.
- **SS** — auto-syncs continuously from decoded FT8 signals. Each slot, every successfully decoded station contributes a timing sample; a robust outlier-rejecting average (median ± a tolerance window) across all of them sets the correction, so one bad decode can't throw off the reading. The box border flashes bright blue for ~30 ms each time a fresh measurement lands — a visual heartbeat that sync is active — and the hint below reads **"Flash: FT8 synced..."**. A blue frame means it is actively tracking; tap SS to lock it.
- **Apply** — if only SS was synced (HH and MM untouched), `time_sync_apply_correction_ms()` nudges the system clock by the measured sub-second offset and writes through to the RTC. If HH or MM was edited, `time_sync_set_manual()` sets the full time. The bottom bar shows **UTC(FT8)** after an FT8-derived correction, or **UTC(manual)** after a full manual set.

---

## Reference

### Gestures

| Gesture | Where | Effect |
|---------|-------|--------|
| Tap | Spectrum / waterfall | Tune to tapped frequency (snapped) |
| Touch + drag (hold ~250 ms first) | Spectrum / waterfall | Cyan cursor snaps grid-to-grid; tunes on release |
| Fast one-finger horizontal swipe | Spectrum / waterfall, any zoom | Pan/"stroll" the view; retunes to the new centre on release |
| Double-tap | Spectrum / waterfall | Reset zoom and pan to ×1.0 / centred |
| Pinch (two fingers) | Spectrum / waterfall | Zoom ×1.0–×24.0 |
| Two-finger drag | Spectrum / waterfall (zoomed) | Pan the zoomed window |
| Swipe → from left edge | Left edge strip | Toggle Panadapter ↔ FT8 screen |
| Swipe ← from right edge | Right edge strip | Open settings drawer |
| Tap right grip handle | Right edge | Open settings drawer (alternative) |
| Swipe → | Open drawer, or spectrum while drawer open | Close settings drawer |
| Swipe ↑ from bottom edge | Bottom edge strip | Open memory channel picker |
| Tap or swipe ↓ | Top-bar item (Band/Mode/BW/Freq/Zoom) | Open that item's selector |
| Touch and hold ~250 ms | FT8 decode list row | Dim preview at ~80 ms; full selection at 250 ms (scroll locks, drag moves highlight, lift to confirm) |
| Quick swipe | FT8 decode list | Scroll the list normally |

### Per-unit IF calibration

The QMX's +12 kHz IF injection varies slightly between units. If signals appear consistently shifted left or right of where the QMX is actually tuned, open the settings drawer → **IF calibration** slider (±100 Hz, persisted to NVS). Default 0. Reported by Ken KF0AYY, whose unit needed about −55 Hz to centre correctly.

### Hardware

- **M5Stack Tab5** — ESP32-P4 v1.3 (ECO2), ST7121 or ST7123 5" 720×1280 MIPI-DSI touch display, 32 MB PSRAM, ESP32-C6 co-processor for WiFi
- **QRP Labs QMX or QMX+** — firmware 1.03.002 or newer (the 1.04.002 beta also works, and unlocks AM mode + Antenna Tune)
- **USB-A → USB-C data cable** between Tab5 USB-A host port and QMX (full data, not charge-only)
- **USB-C power supply** for the Tab5 (5 V / 2 A or better, or internal battery)

```
┌──────────────┐   USB-A → USB-C (data cable)   ┌─────────────┐
│  M5Stack     │ ──────────────────────────────► │   QMX+      │
│  Tab5        │   UAC: I/Q audio (48 kHz stereo)│             │
│              │   CDC-ACM: CAT control          └─────────────┘
│              │
│              │   USB-C (any power cable)       ┌─────────────┐
│              │ ──────────────────────────────► │  5 V source │
└──────────────┘                                 └─────────────┘
```

### What works without the QMX connected

| Works without QMX | Needs QMX connected |
|-------------------|---------------------|
| UI, settings drawer, gestures | Spectrum + waterfall (no I/Q = flat line) |
| WiFi, web UI, screenshots | Top bar Band / Mode / BW |
| Callsign / grid / WiFi credentials | Tap-to-tune, mode popup (CAT writes) |
| Brightness, colour map, dB range | S-meter, FT8 decode/TX, QMX firmware readout |

If the spectrum is flat and the top bar shows `---`, that is almost always the radio not being seen over USB — which loops back to the cable (or the QMX being off / in flash mode).

### Spectrum signal shifted / mirrored, tunable across the whole 48 kHz window

This means the QMX never confirmed IQ mode for the session — without it the radio streams plain (non-IQ) audio instead of a centred baseband, so the signal appears at the wrong place and slides across the full 48 kHz window as you tune. As of v0.19.3 the Tab5 retries the IQ-mode handshake automatically at connect (up to 4 attempts) and this resolves itself almost every time; if it still fails after all retries, a red banner appears across the top of the screen telling you so immediately, instead of leaving you to figure it out from a shifted waterfall. If you see the banner, power-cycle the QMX (forces a fresh handshake on reconnect) or check the QMX's own **System Config → IQ Mode** setting.

### Reporting hardware issues

Near the top of the boot log you will see:

```
I (xxxx) bsp_info: === TAB5 BSP INFO ===
I (xxxx) bsp_info: chip:     ESP32-P4 rev v1.3
I (xxxx) bsp_info: psram:    30 MB
I (xxxx) bsp_info: panel:    ST7123 (inferred from touch)
I (xxxx) bsp_info: touch:    ST7123 @ 0x55
I (xxxx) bsp_info: heap:     230.5 kB internal free, 28.80 MB PSRAM free
I (xxxx) bsp_info: idf:      v5.4.4
I (xxxx) bsp_info: firmware: v0.21.0
I (xxxx) bsp_info: =====================
```

When opening a hardware issue, paste this block — the `panel` and `touch` lines are the first thing needed to identify which Tab5 hardware revision you have.

> **Note on `reset_reason`:** a deliberate reset-button force-off returns `panic/exception`, not `external-pin`/`power-on`. A genuine firmware crash always prints a `Guru Meditation` register + backtrace dump immediately *before* the reboot. No backtrace = abrupt power-off, not a crash.

---

<!-- USERGUIDE:END -->

## Build from source

Requires **ESP-IDF v5.4.4** — pinned; do not upgrade. ESP32-P4 v1.3 silicon requires `CONFIG_ESP32P4_REV_MIN_0=y` and CPU capped at 360 MHz, both baked into `sdkconfig`.

```powershell
# Activate the IDF environment, then:
idf.py build flash monitor
```

Or with the `qmx` PowerShell helper — add to your `$PROFILE` and adjust the COM port:

```powershell
function qmx {
    param([string]$cmd = "fm")
    if (-not $env:IDF_PATH) { & C:\esp\v5.4.4\esp-idf\export.ps1 }
    switch ($cmd) {
        "b"   { idf.py build }
        "f"   { idf.py flash }
        "m"   { python -m esp_idf_monitor -p COM3 build\qmx_panadapter.elf }
        "fm"  { idf.py flash; python -m esp_idf_monitor -p COM3 build\qmx_panadapter.elf }
        "bfm" { idf.py build flash; python -m esp_idf_monitor -p COM3 build\qmx_panadapter.elf }
    }
}
```

Exit monitor: `Ctrl+T` then `Ctrl+X`.

---

## Under the hood

### I/Q balance correction

The QMX presents I/Q audio with a small but measurable amplitude imbalance and quadrature error between channels. Without correction, every signal produces a mirror image across the centre frequency (~30 dB weaker), cluttering the waterfall on a busy band.

A blind adaptive Gram-Schmidt orthogonaliser runs sample-by-sample in the audio pipeline before the FFT. It maintains running estimates of:
- **DC offset** on each channel (τ = 1 s)
- **Power** in I and Q (τ = 200 ms)
- **Cross-product** I·Q (τ = 1 s) — non-zero cross-product means the channels are not orthogonal

Correction per sample:

```
i_out = i
q_out = (q − K_phi · i) × K_amp
```

No calibration step needed; the estimator converges on ambient band noise within a few seconds of any signal. A two-speed startup runs all alphas at 8× for the first 2 s of real signal after each reset (effective convergence ~125 ms), then drops to steady-state. Toggle in the settings drawer; re-enabling resets the estimator so it reconverges from a clean state.

### DSP pipeline

- **FFT:** 1024-pt complex, Blackman-Harris window. esp-dsp ANSI fallback is used (the PIE/vector version crashes under sustained WebSocket load on this silicon).
- **IF offset:** The QMX presents I/Q at +12 kHz; spectrum, waterfall, and S-meter all shift bin selection by `n_bins/4` so the VFO signal appears at the visual centre.
- **DC blocker:** one-pole IIR on the I/Q stream before FFT.
- **Spectrum smoothing:** per-bin EMA (α = 0.4 default, adjustable).
- **dBm calibration:** `DSP_DB_CALIBRATION_OFFSET = −148.0 dB` measured on dummy load; noise floor reads −130 dBm; S9 = −73 dBm.
- **Waterfall scroll:** 1280×824 double-height canvas (~130 µs/tick vs ~92 ms with memmove).
- **Audio task:** polling on core 0 with a drain loop. Event-driven reads caused noise-floor pumping (~13 s cycle) from truncated UAC chunks; this architecture eliminates it.

### Quirks and trade-offs

**PI4IO expander must be initialised before display bring-up.** The Tab5's PI4IO I/O expander holds `LCD_RST` and `TP_RST` low at chip power-on. Without explicit init, on a true cold boot the panel never comes out of reset and the DSI FIFO hangs. Soft resets mask this because the expander retains state across ESP32 resets. `display_init` calls `bsp_i2c_init()` + `bsp_io_expander_pi4ioe_init()` first, then waits 120 ms.

**Patched components in `components/`.** `espressif__usb_host_uac/` has `create_background_task = true` — required for UAC and CDC-ACM to coexist on the same USB host. `espressif__esp_lcd_touch_st7123/` makes `FW_VERSION_REG` the only mandatory register read (ST7121 NACKs the others and also adds a `max_touches > 10` bounds clamp). Do not replace either with the registry version without re-applying the patches.

**LVGL software rotation (~50% FPS cost).** The panel is natively portrait; landscape is achieved via `lv_display_set_rotation(LV_DISPLAY_ROTATION_90)`. Every flush goes through `rotate90_rgb565`. ~13 fps landscape vs ~22 fps portrait — acceptable for a panadapter. PPA hardware rotation conflicts with the USB host stack over DW-GDMA channels and silently kills UAC + CDC-ACM; do not set `CONFIG_LVGL_PORT_ENABLE_PPA=y`. A full native-portrait rewrite would recover the lost FPS but isn't planned — accepted as a permanent trade-off.

**IDLE watchdog disabled.** The LVGL rotation pipeline keeps CPU0 busy past the default watchdog window. `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/CPU1` are off; the app-task watchdog (30 s) is still active.

**Cross-thread CAT writes must go through the poll task.** Writing CAT commands directly from the LVGL thread races the FA/MD/FW poll on the same CDC pipe — commands interleave and the QMX returns `?;`. Pattern: stash the request in a `volatile` and let `poll_task` drain it on its next cycle. See `s_pending_ssb_bw` / `cat_request_ssb_bandwidth()` in `cat.c`.

**SSB filter bandwidth needs three coordinated writes.** `MMSSB|Filter RX=<hz>;` commits the value (persists, shows in QMX menu); `MMSSB|Bandwidth=<hz>;` applies it live; `FW;` polling must be suspended while the width is pinned because reading the filter makes the QMX revert it. Dead ends: `FW<nnnn>;` CAT set returns `?;`; re-asserting the same mode digit does not reload the filter; `Bandwidth` write alone reverts on the next poll.

**HTTPS needs mbedtls allocating from PSRAM, not internal RAM.** The QRZ/eQSL upload features (v0.16.2) are this firmware's first-ever outbound HTTPS connections — SNTP, the only prior network client, is UDP. With the default `CONFIG_MBEDTLS_MEM_ALLOC_MODE=MBEDTLS_INTERNAL_MEM_ALLOC`, the TLS handshake's 16 KB+ buffers competed for the ~200 KB internal DRAM already under pressure from USB host/audio/FFT/LVGL and every connection failed with `ESP_ERR_HTTP_CONNECT`. Fixed by switching to `MBEDTLS_EXTERNAL_MEM_ALLOC` in `sdkconfig`, which moves those buffers into the 28 MB of free PSRAM instead.

### Project layout

```
main/
  main.c                    app_main, task launch, orchestration
  display/display.c         BSP bring-up (PI4IO + LCD + touch)
  ui/ui.c                   LVGL widgets, touch handler, canvases
  ui/ft8_screen_view.c      FT8 decode list, touch-drag row selection
  ui/ft8_tx_modal.c         TX confirmation modal
  cat/cat.c                 USB CDC-ACM + Kenwood CAT
  audio/audio.c             USB UAC + ring buffer producer
  dsp/dsp.c                 FFT, spectrum mutex, DC blocker
  dsp/iq_balance.c          Gram-Schmidt I/Q correction
  ft8_tx.c                  FT8 TX engine (build/arm/run/abort)
  ft8_test.c                FT8 slot loop (RX decode / TX burst)
  ft8_qso.c                 Auto QSO state machine
  ft8_sim.c                 FT8 simulation mode: phantom-station practice QSOs
  ft8_status.c              Mutex-protected FT8 status string
  adif/adif_log.c           ADIF QSO logging
  adif/qrz_upload.c         QRZ Logbook API upload (one record per request)
  adif/eqsl_upload.c        eQSL.cc upload (batched, multiple records per request)
  rtc/rtc.c                 RX8130CE supercap RTC driver
  time_sync/time_sync.c     Time sync orchestrator
  render/render.c           30 Hz render task, EMA smoothing
  render/render_waterfall.c Waterfall scroll (double-height canvas)
  screenshot/screenshot.c   RGB565 framebuffer capture for /ss.bmp
  storage/settings.c        NVS-backed settings with debounced flush
  wifi/wifi.c               C6 co-processor WiFi + SNTP
  net/webserver.c           HTTP server + WebSocket
  util/fps.c                FPS counter
  util/diag_log.c           Diagnostic log ring buffer (vprintf hook)
```

---

## Roadmap

The full per-version changelog — every release from v0.1.0 onward — lives in **[docs/version-history.md](docs/version-history.md)**. This section tracks only what's planned next.

### Next up

**v1.6.0 is here** — **the browser became a second operating position, and the manual stopped being made of characters.** The web page can now work the band rather than watch it: reply to a station, pick your TX tone, edit settings and memory channels, read the whole manual — all served by the Tab5 itself. It answers to **`qmx.local`** so its IP stops mattering. For CW there is a **transmit offset** so a QRP call is not buried in the zero-beat pile (Roy KI0ER), plus **RF gain** and a **Release radio** button for using the QMX's own menus (Stan KC7XE). A radio that stops sending audio now recovers itself. The manual gained an **A–Z index** and properly drawn diagrams, and the settings drawer is grouped with a **Basic/Expert** toggle. Next on the bench:

- **Web-UI audio streaming.** Listen to the receiver in any browser on your LAN — demodulated on the Tab5, no PC. Already working in development; held back for quality tuning and an overnight streaming soak. Server mode (screen off, device just serves) rides along.
- **CW page.** Canned-message CW TX memories first; decoded-CW display after (the QMX decodes internally — mirroring it over CAT looks cheap).
- **Binaural CW audio.** Asked for by Roy KI0ER, and shaped by Don N2VGU and Michael KZ4LY: a stereo sound stage so two stations a few tens of hertz apart land in different places in your head, with the **stage width a setting** rather than a fixed angle. The DSP is small — the Tab5 already receives I and Q separately — but it needs the Tab5's own audio output path, which is the same rework the CW page waits on.
- **Tab5 audio output rework.** The blocker under both of the above: the output task must not run at all in FT8/FT4 (Michael KZ4LY's suggestion), since merely existing at a higher priority than the FFT consumer cost decode yield.

### Longer term

- **CW decoder.** Goertzel-based, text scrolling under the spectrum. The QMX already does this internally — question is whether to mirror its output via CAT or run a parallel decoder on the Tab5.
- **Tab5 speaker / headphone audio.** Demodulated CW/SSB passband audio out of the Tab5's own jack, so the operator can monitor without the QMX's audio path.
- **Extended waterfall history.** PSRAM has room for several minutes of scrollback; two-finger drag to scrub through history.
- **QMX (small) support.** Same UI, different USB endpoint config and band table.
- **JS8 / RTTY modes.** See `docs/js8-feasibility.md` and `docs/rtty-feasibility.md`.
- **DSP polish.** Noise reduction, auto-notch.

---

## Related projects

- [DX-FT8](https://github.com/WB2CBA/DX-FT8-FT8-MULTIBAND-TABLET-TRANSCEIVER) by Barb (WB2CBA) — open-hardware FT8 tablet transceiver; an inspiring reference for a similar use-case
- [`qrp_companion`](https://groups.io/g/QRPLabs/topic/118645485) by Zhenxing Han (N6HAN) — Tab5 companion for QMX with audio + CAT; source of the polling audio task pattern and battery readout approach
- [`ft8_lib`](https://github.com/kgoba/ft8_lib) by Karlis Goba — FT8 encoder/decoder vendored as `components/ft8_lib`

---

## Glossary

**Common terms and acronyms used in this guide:**

| Term | Meaning |
|------|---------|
| **CAT** | Computer-Aided Transceiver — radio control protocol (Kenwood-style commands via serial/USB) |
| **CDC-ACM** | Communications Device Class / Abstract Control Model — USB standard for serial ports |
| **CQ** | General call to any station (not directed at anyone specific) |
| **CW** | Continuous Wave — Morse code mode |
| **DSP** | Digital Signal Processing — mathematical signal analysis and filtering |
| **FFT** | Fast Fourier Transform — algorithm to convert time-domain audio into frequency spectrum |
| **FT8 / FT4** | Digital modes for weak-signal HF communication (15-second vs 7.5-second slots). Both fully supported (FT4 re-enabled in v0.21.0). |
| **GPIO** | General-Purpose Input/Output — microcontroller pins for digital signals |
| **I2C / SPI** | Serial communication protocols for connecting peripherals (sensors, displays, etc.) |
| **IQ** | In-phase / Quadrature — stereo representation of RF signals (real + imaginary parts) |
| **LVGL** | Light and Versatile Graphics Library — open-source embedded UI toolkit used for the display |
| **NVS** | Non-Volatile Storage — persistent memory on the ESP32 (survives power cycles) |
| **PSRAM** | Pseudo-SRAM — extra RAM on the Tab5 (used for large buffers like waterfall history) |
| **QMX / QMX+** | QRP Labs HF transceiver — the radio this panadapter controls and receives audio from |
| **QSO** | Radio contact / conversation between two stations |
| **RTC** | Real-Time Clock — battery-backed timer on the Tab5 (keeps time during power-off) |
| **SNTP** | Simple Network Time Protocol — synchronizes system clock via WiFi/internet |
| **SWR** | Standing Wave Ratio — antenna impedance matching metric (1.0 = perfect) |
| **TX / RX** | Transmit / Receive — keying the radio and listening |
| **UAC** | USB Audio Class — standard for streaming audio over USB |
| **USB** | Universal Serial Bus — physical connector and protocol (carries both audio and CAT commands) |
| **UTC** | Coordinated Universal Time — timezone-independent time standard for FT8 slot alignment |
| **VFO** | Variable Frequency Oscillator — the radio's tuning dial / frequency setting |

---

## Contributing

This is a solo project — all coding is done by the author (OZ1LAV) at his own pace, and **pull requests are not being accepted right now**. Bug reports, feature requests, and field reports are very welcome, though, via [GitHub Issues](https://github.com/SteffenLav/qmx-panadapter/issues) or the [QRPLabs Groups.io thread](https://groups.io/g/QRPLabs/topic/119565643). See [CONTRIBUTING.md](CONTRIBUTING.md) for details.

## License

MIT (see LICENSE). Copyright © 2026 Steffen Lav (OZ1LAV).
