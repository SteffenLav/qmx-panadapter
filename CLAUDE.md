# CLAUDE.md — QMX Panadapter

ESP-IDF firmware for M5Stack Tab5 (ESP32-P4). Real-time panadapter for the QRP Labs QMX/QMX+ HF transceiver: USB host captures IQ audio (UAC) + CAT (CDC-ACM), FFT runs on-device, spectrum + waterfall rendered on the 5" touch display (landscape 1280×720).

## Build & flash

```powershell
idf.py build flash monitor          # standard IDF
qmx bfm                             # PowerShell helper (build + flash + monitor)
qmx fm                              # flash + monitor (skip rebuild)
qmx m                               # monitor only
```

Exit monitor: `Ctrl+T` then `Ctrl+X`.

**Pinned to ESP-IDF v5.4.4.** Do not upgrade — ESP32-P4 v1.3 (ECO2) silicon requires `CONFIG_ESP32P4_REV_MIN_0=y` and CPU capped at 360 MHz, both baked into `sdkconfig`.

## Module map

```
main/
  main.c                  app_main, task launch, orchestration
  display/display.c       BSP bring-up (PI4IO expander + LCD + touch)
  ui/ui.c                 LVGL widgets, touch handler, spectrum/waterfall canvases
  cat/cat.c               USB CDC-ACM + Kenwood CAT (FA/MD/FW poll, tune write)
  audio/audio.c           USB UAC + ring buffer producer (core 0, polling)
  dsp/dsp.c               FFT consumer (reads ring buffer), spectrum mutex, DC blocker
  render/render.c         30 Hz render task, EMA smoothing, dB scaling
  render/render_waterfall.c  Waterfall tick, double-height canvas scroll trick
  screenshot/screenshot.c UART screenshot dump (hidden long-press, top-left 80×80)
  util/fps.c              FPS counter
```

Data flow: **audio → ring buffer → dsp (FFT) → spectrum mutex → render → LVGL canvases**

## Critical quirks

### PI4IO expander must be initialized before display bring-up
`display_init` calls `bsp_i2c_init()` + `bsp_io_expander_pi4ioe_init()` first, then waits 120 ms. Skipping this causes a cold-boot hang in the DSI FIFO loop. Soft resets mask the bug because the expander retains state across ESP32 resets.

### Patched UAC component in `components/`
`components/espressif__usb_host_uac/` is a hand-patched fork with `create_background_task = true`. This is required for UAC + CDC-ACM to coexist on the same USB host. Do not replace it with the registry version without re-applying the patch.

### LVGL software rotation (~50% FPS cost)
The ST7123 panel is natively portrait; landscape is achieved via `lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)`. Every LVGL flush goes through `rotate90_rgb565`. FPS is ~13 landscape vs ~22 portrait. Acceptable for a panadapter; Phase 6.3 plans native-orientation rendering to recover the loss.

### IDLE watchdog disabled
`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/CPU1` are off. The LVGL rotation pipeline keeps CPU0 busy past the default watchdog window. App-task watchdog (30 s) is still active.

### Audio task is polling, not event-driven
`audio_task` runs on core 0 with `uac_host_device_read` + a drain loop. Event-driven reads caused noise-floor pumping (slow ~13 s cycle) due to truncated UAC chunks saturating the FFT input. Do not revert to event-driven.

### Waterfall double-height buffer
`1280 × 824` RGB565 canvas (2× waterfall height). Each tick writes the new row at `s_wf_head` and `s_wf_head + WATERFALL_H`, then moves the view pointer. Avoids `memmove` (~130 µs vs ~92 ms per tick).

### 12 kHz IF offset
The QMX presents IQ with +12 kHz IF offset — the VFO signal lands at +12 kHz in baseband. Spectrum and waterfall shift bin selection by `n_bins/4` to center the VFO signal visually. Touch-to-tune math uses the raw CAT frequency, so no adjustment needed there.

## Display layout (landscape 1280×720)

```
+----------------------------------------------------------+
| Top bar 60 px   freq | mode | s-meter | burger (80×80)  |
+----------------------------------------------------------+
| Spectrum 200 px — green curve, amber VFO, grey passband  |
+----------------------------------------------------------+
| Freq axis 18 px — absolute MHz labels                    |
+----------------------------------------------------------+
| Waterfall 412 px — newest row at top, SDR gradient       |
+----------------------------------------------------------+
| Bottom bar 30 px — status / span / fps                   |
+----------------------------------------------------------+
```

Top-right 200×120 of the spectrum is a deadzone (suppresses accidental tunes behind the burger button).

## CAT

Kenwood-style. Round-robin poll: `FA` (freq) / `MD` (mode) / `FW` (passband width), 50 ms intervals — each field refreshes every ~150 ms. CAT writes rate-limited to one per 200 ms. Touch-to-tune optimistically updates the freq label on write without waiting for the next FA poll.

## dBm calibration

`DSP_DB_CALIBRATION_OFFSET = -148.0 dB` (measured on dummy load, noise floor → -130 dBm). S9 = -73 dBm. Display range: -130 to -30 dBm.

## Branch state

| Branch | What | State |
|--------|------|-------|
| `main` | All phases through 5.11 + Phase A IQ balance + cold-boot fix | stable |

Phase B will add a settings drawer toggle for IQ balance; Phase C will tune time constants.
