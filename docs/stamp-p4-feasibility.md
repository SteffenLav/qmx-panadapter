# Porting to the M5Stamp ESP32-P4: what it would take

**Question asked by Randy N4OPI, 2026-10-13:** put an ESP32-P4 stamp inside the
QMX+ that serves only the web UI, with no display at all. Is that a port, or a
second product?

**Short answer: it is a port, not a second product - and the hardware could
hardly be better suited. But the code is not ready for it, and the obstacle is
not the display.**

Two things live inside the UI layer today that a headless build cannot do
without: the **radio's own state** (what frequency, band, mode, zoom and
passband the radio is on) and the **scheduler** that executes everything the
browser asks for. Until those are lifted out, a display-less build renders a
perfect web page that does nothing at all.

That extraction is a staged refactor of the largest file in the project,
measured in weeks rather than days. It is worth doing even if the stamp never
happens - it is the same prerequisite as a Tab5 "web-only" mode.

⚠ Everything below is MEASURED against the tree at `v1.18.0-16`. Where the
first version of this document guessed, §2 says so and gives the real number.

---

## 1. The hardware is the same machine

| | M5Tab5 (today) | Stamp-P4 (S013) + AddOn C6 (A172) |
|---|---|---|
| SoC | ESP32-P4 | ESP32-P4NRW32 - **same** |
| Flash | 16 MB | **16 MB** |
| PSRAM | octal | **32 MB octal** |
| Wi-Fi | C6 over SDIO | C6-MINI-1-N4 over **SDIO** - same transport |
| USB host | yes | **yes** - USB2 OTG D+/D- on stamp holes |
| Display | 1280x720 MIPI DSI | none (2-lane DSI is led out, unused) |

⭐ **The consequences of that table are the whole answer.** The application
binary is 5.18 MB and the partition table (`ota_0` 8 MB, `ota_1` 6.8 MB,
`storage` 1 MB SPIFFS) fits 16 MB unchanged. The USB host peripheral - which
is the entire QMX link, UAC audio plus CDC-ACM CAT - is the same peripheral.
The Wi-Fi path is `esp_hosted` over SDIO on both, so the throughput ceiling and
its quirks carry over exactly, including the one measured in
`docs/remote-iq-assessment.md`.

⭐ Nothing in the signal path needs porting at all. ⛔ But "compile it for a
board without a screen" is NOT what this is - see §4 and §5.

## 2. ⛔ A CORRECTION TO THE FIRST VERSION OF THIS DOCUMENT

The first cut of this file, written 2026-10-14, carried four numbers that were
wrong because they came from narrow greps. They are corrected below and the
originals are kept here, because the wrong ones all pointed the same way: they
made the port look **smaller** than it is.

| First cut said | Measured |
|---|---|
| the web server needs **12** symbols from the UI layer | **112** UI-layer symbols; the net layer alone uses **57 of the 114** public accessors in `ui.h` |
| `main/ui/` "simply does not compile in" | **false** - most of `ui.c` is not widget code and the web UI depends on it |
| **6** `ui_mode_get()` sites in engine code | **14** |
| **9** `ui_toast()` sites in engine code | **23** |
| "a day or two of careful surgery" | **retracted** - see §5 |

The first three came from grepping `main/*.c` and one symbol prefix, missing
`main/net/`, `main/dsp/`, `main/adif/`, `main/wifi/`, `main/cat/` and
`main/render/` entirely.

## 3. What ports unchanged

About half the tree, and it is the half that took the longest:

- USB host: UAC audio capture and CDC-ACM CAT. `main/audio/audio.c` drives the
  UAC device directly and does not depend on the Tab5's own codec
- the DSP: FFT, filters, demodulation
- every decoder and encoder: FT8, FT4, JS8, WSPR, CW
- the QSO machine, the robot, the pileup, grey-listing, ADIF
- the web server and the web UI page itself
- settings/NVS, SPIFFS, OTA, update checking, PSKReporter, DX cluster

## 4. What is genuinely lost, and what only LOOKS lost

⚠ **"Not on the module" and "gone" are different things, and the first cut of
this document ran them together.**

**Gone, because they depend on a screen:** the display and touch, the
on-screen keyboard, the BLE HID mouse and cursor, and `/ss.bmp` screenshots -
which are also a diagnostic tool, so that one costs support as well as
convenience.

**Not fitted, but addable as carrier-board parts.** The Stamp-P4 carries no
audio codec, amplifier, headphone jack, microphone or card slot (M5Stack's own
spec page lists none of them), and no battery gauge or PMU. But the P4's
peripherals are still there behind the 44 exposed GPIOs, so a carrier could add
them. Inside a QMX+ most would be pointless: you listen on the radio.

⭐ **The one genuine capability loss in that group is the LINE-IN path** - the
3.5 mm jack is ES7210 channel 3, and it is how audio from a non-USB radio or a
second receiver gets in. A stamp build is USB-only.

⛔ **The SD card deserves care, and "it loses the SD" was too glib.** The
ESP32-P4 has **two SDMMC slots**, both routable through the GPIO matrix, so a
card CAN be wired to the stamp's pins. The real problem is contention, and this
project already has it on the Tab5: `settings.c` records that "SD card I/O
competes with the WiFi co-processor's SDIO link for the same shared SDMMC host
peripheral", and an unnecessary 15-second SD write was found to be "a standing,
unintentional trigger for that same hazard". On a stamp, where the C6 takes the
BTB SDIO interface and Wi-Fi is the entire point of the board, that contention
is something to design away from rather than into.

**And the MBUS Module GPS**, which has the clean answer below.

⭐ The Module GPS has a clean answer, and it is the OPERATOR's point, not
Randy's - Randy never mentions GPS anywhere in his message. The **QMX+ internal
GPS** replaces it, and that path already ships: the dev bench runs on it today
and reports `QMX GPS source: QMX+ Internal`.

⛔ **What only LOOKS lost is the hard part.** `main/ui/ui.c` is **17,582
lines - the largest file in the project - and only 19% of its lines touch
`lv_` at all.** The other ~14,000 lines are not widgets. They are the radio's
own state and logic:

    ui_get_dial_freq_hz   ui_update_frequency   ui_validate_band_freq_hz
    ui_get_band_str       ui_get_mode_str       ui_band_last_hz
    ui_set_zoom           ui_get_zoom_factor    ui_get_pan_offset_bins
    ui_get_db_range       ui_get_passband_edges_hz
    ui_get_if_cal_hz      ui_get_if_bin_shift   ui_set_cat_paused

The browser reads and writes all of that. **`/api/status` cannot answer what
frequency the radio is on without `ui.c`.** So "the UI half does not compile
in" - the claim that made the first version of this document optimistic - is
simply false. A headless build needs most of that file.

## 4a. ⛔ THE AUDIO QUESTION, WHICH IS THE BIGGEST ONE

⛔ **The first version of this document wrote off local audio with "you listen
on the radio". That is wrong, and it is the most important error in it.**

The Tab5 is not a screen bolted to a receiver. It is an audio renderer, and
some of what it renders the radio cannot produce at all:

- **Panoramic CW is SHIPPED** (`rx_audio.c`, 2026-09-20). It splits the CW
  passband into its lower and upper halves and pans each hard L/R, so two CW
  stations sitting either side of the tuned pitch separate **spatially**
  instead of landing on one mono note. It is switchable from the Tab5's
  resource modal and from `{"binaural":true}` over the API, and the pan shape
  has its own persisted sliders. **A QMX cannot do this. The Tab5 does.**
- **The RX audio path** generally - listening to CW and SSB through the Tab5's
  own filtering rather than the radio's.
- **Line-in** (the 3.5 mm jack, ES7210 channel 3) for a non-USB source.
- **AM demodulation** - Sam W7STF's one genuine want, and the Tab5 is silent
  in AM today. If it is implemented, the audio has to come out somewhere.

⛔ **A headless stamp has no audio output at all.** So on that board, every one
of those either does not exist, or it goes out over the network. There is no
third option.

⭐ **Which means A1 web audio stops being a wish and becomes the stamp's only
possible audio path.** If the stamp is a direction, the roadmap order changes:
web audio is no longer "the thing Sam has been waiting for since July", it is
the prerequisite for a stamp having any audio at all - including AM, if that
gets built.

⚠ **And binaural does not ride along for free.** `docs/web-audio-feasibility.md`
plans **mono** - µ-law at 12 kHz, **96 kbps** - and its own risk section calls
the combined ~180 kbps with the spectrum stream "a duty cycle it has never
soaked under". Panoramic CW is a **stereo** feature: carrying it doubles the
audio payload to ~192 kbps, roughly **274 kbps combined**, over the same
SDIO-to-C6 link that is already the measured bottleneck. That wants soaking
before anybody promises spatial CW in a browser.

## 4a-bis. ⭐ THE OPERATOR'S INVERSION: SERVE I/Q, DO THE DSP ON THE PC

The audio problem in §4a assumes the stamp renders audio and streams the
RESULT. The operator's point, 2026-10-14, is that this is the wrong way round
for a board whose only purpose is to serve a client: **stream the I/Q and let
the PC do everything.**

⭐ **That dissolves §4a entirely.** Panoramic CW, AM demodulation and Sam
W7STF's DNR (he means LMS and spectral subtraction) all become client-side DSP
on a machine with no CPU budget worth worrying about - and
`docs/web-audio-feasibility.md` already notes DNR can run client-side as
RNNoise-class WASM. The device needs no audio output and no audio DSP at all.

⭐⭐ **And it dissolves the main objection in `docs/remote-iq-assessment.md`
too.** That document REJECTED "streaming I/Q while the normal UI still runs",
because "the device is already at 0.0-0.2% idle on core 0 with the radio
streaming, and an open browser already breaks the audio". **A stamp has no
LVGL, no render task and no software rotation** - exactly the load that forced
I/Q streaming to be a special boot mode on a Tab5. On a stamp it is not a mode,
it is the whole machine.

So the stamp is not "a Tab5 with the screen removed". It is the I/Q-server
configuration that assessment already designed, running natively.

### What this does to the size of the job

It splits the stamp into two quite different products:

| | what the device does | what it costs |
|---|---|---|
| **A. RX pipe** | USB host, CAT, WiFi, a websocket sink. No DSP, no decoding, no audio | the I/Q doc calls streaming mode "a branch in `app_main` and a socket". **The §5 refactor is largely NOT needed**: the panadapter, zoom and passband state the browser reads from `ui.c` is replaced by the PC computing its own |
| **B. Headless station** | decodes and transmits on its own, web UI as today | the full §5 extraction, weeks, large blast radius |

⛔ **TX is what keeps B expensive, and it cannot be wished away.** The I/Q
assessment is explicit: CW keying is ~60 ms per element at 20 WPM and "not
remotely schedulable"; an FT8 burst must be "armed and run locally" to slot
accuracy. So anything that transmits keeps CAT, the TX engines and their
schedulers on the device - and those schedulers are exactly what §5 found
living on the LVGL timer. A pure RX pipe sidesteps that; a transmitting stamp
does not.

### The gate, unchanged and still unmeasured

**Can `esp_hosted` -> C6 -> lwIP -> httpd websocket sustain ~1.5 Mbit/s with
low enough jitter?** Nobody has measured it, on either board.

| | rate |
|---|---|
| raw 16-bit, 48 kHz | 1.54 Mbit/s |
| 12-bit block-float packing | ~1.15 Mbit/s |
| decimated to +/-12 kHz | 768 kbit/s |
| both | ~580 kbit/s |
| fallback: downconverted passband only | ~192 kbit/s |

⚠ A dropout does not glitch the audio, it loses a decode.

⭐ **One system-of-record decision comes with it**, and the I/Q assessment
already picks it: the browser posts decodes and QSOs BACK, so WSPR TX, PSK
Reporter and Cloudlog keep running unattended. If those only work while a
browser is open, the product is a downgrade.

## 4b. How much code actually moves

⚠ **Counted with comments and blank lines EXCLUDED.** This project is 33%
comments, so a raw line count (~186,000) flatters every answer here. The code
figures:

| | code lines |
|---|---|
| `main/` (everything, including the UI) | 97,600 |
| `components/ft8_lib` (the decoders) | 11,000 |
| `components/m5stack_tab5` (the BSP, replaced for a stamp) | 2,600 |
| **firmware total** | **≈111,000** |
| the web page itself (JS/CSS/HTML in `index.html`) | 6,900 |

And what becomes of it on a display-less board:

| | code lines | what |
|---|---|---|
| **ports untouched** | **≈85,000** | DSP, every decoder, USB host, CAT, web server and page, storage, settings, ADIF |
| **does not compile in** | **≈27,000** | Tab5 widget code - 291 LVGL functions in `ui.c` alone, plus every modal and view |
| **must be pulled out** | **≈2,000-3,000** | the display-free core currently living inside the UI files |

The pull-out, itemised:

- **1,826 lines** are already display-free functions sitting inside UI files:
  148 functions in `ui.c` (1,391 lines) and 36 in `ft8_screen_view.c` (435).
  They move as they are.
- **1,467 lines** in five files touch no LVGL at all - `ft8_screen.c`,
  `gps_page.c`, `help_topics.c`, `js8_page.c`, `ui_mode.c`. They change folder,
  not content.
- **~150-250 lines** have to be dissected out of `t_clock_cb`, which is **752
  lines** and interleaves the browser's scheduler with widget painting inside
  one function.
- **~65 state variables** must be separated from the **183 `lv_obj_t` widget
  pointers** they currently sit beside in `ui.c`.
- the 65 functions the web layer calls are only **554 lines of bodies** in
  total - they are thin accessors over that state, which is why the state is
  the real target and not the functions.

⭐ **So 2-3% of the firmware has to move, and that is the honest headline.**
It is NOT a large refactor by volume.

⛔ **It is a delicate one by position.** That 2-3% lives inside a 17,582-line
file beside 183 widget pointers, and the single function that must be split is
the heartbeat of all FT8, JS8 and WSPR operation on a radio people use every
day. The volume is not the risk; the blast radius is. That - not size - is why
§5 says weeks and staged.

## 5. ⛔ THE REAL WORK, AND IT IS BIGGER THAN I FIRST SAID

Two separate problems, and the second is the expensive one.

**(a) The LVGL timer is the scheduler.** `t_clock_cb()` in
`ft8_screen_view.c` drains, once a second: `web_reply_drain`,
`s_web_cq_pending`, `s_web_ftx_pending`, `s_web_js8_macro`,
`s_web_parity_pending`, `s_web_override_pending`, and `js8_ftx_tick()` which
arms each frame of a running free-text message. Every one defers there **on
purpose**, because `ft8_tx_arm()` blocks and must never run on the HTTP task.
The reasoning is right; the host it chose is a UI timer.

A second coupling of the same kind: the decode loop's **lifetime** is a UI
question - `ft8_test.c:2939` is `while (ui_mode_get() == UI_MODE_FT8)`, and
there are **14** `ui_mode_get()` sites in engine code, including in `dsp.c`,
`spur_map.c` and `render.c`.

⛔ So a naive headless build renders a perfect web UI that **does nothing**:
no decodes, no transmissions, no response to any button.

**(b) The radio's state has to be extracted from the UI.** This is the part the
first version of this document missed entirely. It means pulling a
display-free "radio core" out of `ui.c` (17.5k lines) and `ft8_screen_view.c`
(4.7k lines) - the state, the validation, the band/zoom/passband maths - and
leaving the widgets behind. The surface is large and it is load-bearing: 57
public accessors, 112 referenced symbols, 37 coupling sites in engine code
(14 `ui_mode_get` + 23 `ui_toast`).

⚠ **I previously called this "a day or two". That was wrong and I retract it -
though §4b shows the reason is not the one I gave when retracting it.** The
volume is small: 2,000-3,000 lines move. What makes it weeks is WHERE they sit
- inside the project's single biggest file, the graph's top god-node, beside
183 widget pointers, with the scheduler tangled into a 752-line paint function
that every FT8, JS8 and WSPR contact depends on. The honest shape is **weeks,
staged**, with the Tab5 proving each stage - because of blast radius, not
because of size.

⭐ **It is still worth doing on its own merits.** It decouples the radio state
from LVGL, makes the control path testable on a host, and it is the *same*
prerequisite as Bruce N9JCV's "let me switch the web UI off". The stamp would
not be paying for this refactor alone.

## 6. Is it cumbersome to maintain afterwards?

**No - afterwards. Getting there is the cost, not living with it.**

Once a display-free radio core exists, the split is clean: one tree, two
targets, with the widget layer compiled only for the Tab5. There is no
duplicated logic to keep in step, because the core is shared by construction.

The standing risk is **drift**: a feature wired into the Tab5 path that the
headless path silently lacks. That is the same shape as the standing-patch and
unattended-TX problems, and it takes the same answer - a checker in `tools/`
that fails the build when a web action has no headless home. Three such gates
already run in this project and each has caught a real defect.

## 7. Open questions a prototype would answer

1. **The USB connector.** The host pins are led out, but not a connector. Who
   supplies VBUS to the radio, and what does the cable look like inside a QMX+?
2. **Power and RF.** A switching DC-DC and a 2.4 GHz radio millimetres from an
   HF receiver's front end. The QMX+ is the one place this must not add noise.
3. **SDIO throughput on this board.** Same transport as the Tab5, but not the
   same layout; the spectrum WebSocket is the demanding client.
4. **Heat**, in a sealed enclosure, at 360 MHz with Wi-Fi up.

None of those is a software question, and none is answered by reading.

---

## Verdict for Randy

**Architecturally yes, and the hardware could hardly be better suited.** Same
SoC, same 16 MB flash and 32 MB PSRAM, the same USB host peripheral, the same
C6-over-SDIO Wi-Fi (verified in our own sdkconfig). The binary and partition
table fit unchanged.

⭐ **But the right question is which of two products it is**, and the answer
changes the cost by an order of magnitude:

- **An RX I/Q pipe** - USB host, CAT, Wi-Fi, a websocket, and the PC does every
  bit of the DSP, decoding and audio. Small. It also makes the stamp the
  natural home for the remote-I/Q idea rather than a compromised one, because
  it carries none of the UI load that forced that idea into a special boot mode
  on the Tab5.
- **A headless station that decodes and transmits on its own** - this needs the
  radio state and the browser's scheduler lifted out of the UI layer: 2-3% of
  the firmware by volume, but sitting under every FT8, JS8 and WSPR contact.
  Weeks, staged.

⛔ **One measurement gates the first and nobody has taken it:** sustained
websocket throughput and jitter to a PC, ~1.5 Mbit/s raw or ~580 kbit/s packed
and decimated. `docs/remote-iq-assessment.md` says everything else waits on it,
and that is still true.

⛔ **What I would NOT promise:** a date, or that either version is easy. The
first draft of this assessment said "a day or two", and separately wrote off
local audio with "you listen on the radio". Both were wrong.

⭐ **What is fair to say:** the architecture does not stand in the way, nothing
about the radio half needs rewriting, one tree with two targets is reasonable,
and the cheap version of his idea is cheaper than this document first implied.
