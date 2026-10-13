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

**Genuinely gone:** the display and touch, the on-screen keyboard, the BLE HID
mouse and cursor, `/ss.bmp` screenshots (also a diagnostic), the ES7210
mic/line-in, the speaker, the battery gauge and PMU, the SD slot, and the MBUS
Module GPS.

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

**Architecturally, yes - and the hardware could hardly be better suited.** Same
SoC, same 16 MB flash and 32 MB PSRAM, the same USB host peripheral, the same
C6-over-SDIO Wi-Fi (verified in our own sdkconfig, not assumed). The binary and
partition table fit unchanged, and the entire signal path - USB audio, DSP, all
four decoders, CAT, the web server - ports without modification.

**The cost is not the display. It is that the radio's state and the browser's
scheduler both live inside the UI today.** Pulling a display-free core out of
`ui.c` is a weeks-shaped, staged refactor of the largest file in the project,
not a weekend port. Every stage is verifiable on the Tab5 as it goes, and the
project wants the result anyway.

⛔ **What I would NOT promise him:** a date, or that it is easy. The first
version of this assessment said "a day or two" and it was wrong by an order of
magnitude on its central measurement.

⭐ **What is fair to promise:** that it is feasible, that the architecture does
not stand in the way, that nothing about the radio half has to be rewritten,
and that maintaining both from one tree is reasonable once the core is lifted
out.
