# Porting to the M5Stamp ESP32-P4: what it would take

**Question asked by Randy N4OPI, 2026-10-13:** put an ESP32-P4 stamp inside the
QMX+ that serves only the web UI, with no display at all. Is that a port, or a
second product?

**Short answer: it is a port, and the architecture allows it** - but not as the
code stands today. One refactor has to happen first, and without it a headless
build does not merely lose the screen, it does nothing at all. That refactor is
bounded and countable, and it is worth doing even if the stamp never happens.

Everything below is measured against the tree at `v1.18.0-16`, not estimated.

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

Nothing in the signal path needs porting. It needs **compiling for a board
without a screen**.

## 2. What ports unchanged

About 55% of the tree, and it is the half that took the longest:

- USB host: UAC audio capture and CDC-ACM CAT
- the DSP: FFT, filters, demodulation
- every decoder and encoder: FT8, FT4, JS8, WSPR, CW
- the QSO machine, the robot, the pileup, grey-listing, ADIF
- the **entire web server and web UI** - which is the product, for this user
- settings/NVS, SPIFFS, OTA, update checking, PSKReporter, DX cluster

## 3. What does not exist on a stamp

- `main/ui/` - **49,245 lines, 45% of the tree**. It simply does not compile in
- touch, the on-screen keyboard, the BLE HID mouse (`bt_hid_mouse.c`) and the
  cursor (`hid_cursor.c`) - all meaningless with no screen
- `/ss.bmp` screenshots, which are also a diagnostic tool and would be lost
- Tab5 peripherals: the ES7210 mic/line-in, the battery gauge and PMU, the
  speaker, the SD card slot
- the **MBUS Module GPS**. ⭐ Randy's own answer is the right one: the QMX+
  internal GPS replaces it, and that path already ships and runs on the dev
  bench today (`QMX GPS source: QMX+ Internal`)

⚠ **GPIO: the sources disagree and the schematic settles it.** M5Stack lists
the exposed pins as G0-G39, G41, G49, G50, G52; Randy reports G40-G48 and G51.
Both agree there is **no G53 or G54**, so the owed meter-on-GPIO53 feature has
no home on this board - it is ADC2-bound in any case.

## 4. ⛔ THE ONE REAL BLOCKER, AND IT IS NOT THE DISPLAY

**The LVGL 1 Hz timer is the scheduler for everything the browser asks for.**
`t_clock_cb()` in `ft8_screen_view.c` drains, once a second:

| Drained on the LVGL timer | What stops working without it |
|---|---|
| `web_reply_drain` | "work this station" from the browser |
| `s_web_cq_pending` | Call CQ from the browser |
| `s_web_ftx_pending` | JS8 free text |
| `s_web_js8_macro` | the JS8 SNR / GRID buttons |
| `s_web_parity_pending` | CQ slot parity |
| `s_web_override_pending` | mid-QSO overrides |
| `js8_ftx_tick()` | arming each frame of a running free-text message |

Every one of those defers to the LVGL task **on purpose**, because
`ft8_tx_arm()` blocks and must never run on the HTTP task. The reasoning is
sound; the host it chose is a UI timer.

A second, quieter coupling: the decode loop's **lifetime** is a UI question.
`ft8_test.c:2939` is literally `while (ui_mode_get() == UI_MODE_FT8)`, with
further gates at `ft8_test.c:2862`, `ft8_sim.c:611` and three sites in
`wspr_rx.c`. No UI mode, no decoding.

⛔ So a naive headless build produces a web UI that **renders perfectly and
does nothing**: no decodes, no transmissions, no response to any button.

## 5. The refactor, and how big it really is

Split `ft8_screen_view.c` (4,755 lines) along the seam that already exists:

- **control** - the pending flags, the drains, `js8_ftx_tick()`, the QSO calls.
  Moves to a plain FreeRTOS task at 1 Hz. No LVGL.
- **view** - the widgets. Tab5 only.

The surface to move is small and countable:

- the web server needs exactly **12 symbols** from `ft8_screen_view`, of which
  **9 are behaviour** and 3 are UI-state getters
- **6 `ui_mode_get()` sites** in engine files become an explicit "engine
  enabled" state, set by the UI on a Tab5 and by configuration headless
- **9 `ui_toast()` calls** in engine files need a notification shim that logs
  headless and toasts on a Tab5

That is the work. It is a day or two of careful surgery, not a rewrite, and
every step is verifiable on the Tab5 before any stamp exists.

⭐ **It pays for itself without the stamp.** It decouples the QSO machine from
LVGL, makes the control path testable on a host, and is the *same*
prerequisite as Bruce N9JCV's "let me turn the web UI off" request - and of a
Tab5 "web-only" mode.

## 6. Is it cumbersome to maintain?

**One tree, two targets, with one standing risk.**

The 45% that is UI does not compile into the stamp image, so there is no
duplicated logic to keep in step - provided the refactor above is real and the
control path has no second copy. The danger is **drift**: a feature wired into
the Tab5 path that the headless path silently lacks, discovered by a user.

That risk is the same shape as the standing-patch problem and the unattended-TX
problem, and it has the same answer: a checker in `tools/` that fails the build
when a web action has no headless home. This project already runs three such
gates and they have each caught a real defect.

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

It is a port, not a second product. The hardware is the same machine, the
signal path and the whole web UI carry over unchanged, and the only structural
obstacle is that the browser's actions are currently scheduled by a UI timer.
Fixing that is bounded work that the project wants anyway.

⚠ What this does NOT say is when. It says the architecture does not stand in
the way, and that maintaining both from one tree is reasonable if the control
path is lifted out of the view first.
