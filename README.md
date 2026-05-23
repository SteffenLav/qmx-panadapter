# QMX+ Panadapter

A standalone real-time panadapter — spectrum analyzer and waterfall — for the [QRP Labs QMX/QMX+](https://www.qrp-labs.com/qmxp.html) HF transceiver, running on the [M5Stack Tab5](https://docs.m5stack.com/en/core/tab5) (ESP32-P4 with a 5" 720×1280 touch display).

The QMX exposes I/Q audio over USB UAC plus CAT control over USB CDC-ACM. The Tab5 connects to the QMX as a USB host, decodes the I/Q in real time on the ESP32-P4, and renders a touch-driven panadapter with tap-to-tune.

---

## Status

Working. All phases through 6.2 complete, with cold-boot reliability fix.

| Phase | What | Status |
|-------|------|--------|
| 1     | LVGL UI on Tab5 ST7123 display | done |
| 2     | USB Host CDC-ACM, CAT poll, frequency display | done |
| 3     | USB UAC audio streaming + ring buffer + DSP consumer | done |
| 4     | esp-dsp FFT (1024-pt complex, Blackman-Harris) at 48 frames/s | done |
| 5.1   | Real-time spectrum line graph @ 30 Hz | done |
| 5.2   | Waterfall with classic SDR gradient + moving-pointer scroll | done |
| 5.3   | Label band, offset ticks, dB grid | done |
| 5.4   | EMA spectrum smoothing + autoscaling dB range with live labels | done |
| 6.1   | Touch-to-tune via CAT FA, live cyan drag cursor | done |
| 6.2   | Landscape rotation 1280×720 (LVGL software rotation) | done |
| —     | Cold-boot fix (PI4IO expander init for LCD_RST / TP_RST) | done |

Open ideas: manual pre-rotated rendering to recover FPS (6.3), FT8 decoder, memory channels, WiFi station mode.

---

## Hardware

- **M5Stack Tab5** with ESP32-P4 v1.3 (ECO2) silicon, ST7123 5" 720×1280 MIPI-DSI touch panel, 32 MB hex PSRAM
- **QRP Labs QMX or QMX+** transceiver (Kenwood-style CAT, UAC audio)
- USB-C OTG cable Tab5 ↔ QMX

## Software requirements

- **ESP-IDF v5.4.4** — pinned, because:
  - ESP32-P4 v1.3 silicon needs `CONFIG_ESP32P4_REV_MIN_0=y` and CPU capped at 360 MHz
  - The local M5Stack UserDemo BSP is built against this IDF version
- VS Code with the Espressif IDF extension (or any IDF-compatible setup)
- Windows 11 + PowerShell (other platforms work fine, just no `qmx` helper)

## Build, flash, monitor

Standard IDF flow:

    idf.py build flash monitor

Or via the helper function in `$PROFILE` (see Tools section below):

    qmx fm      # build + flash + monitor
    qmx b       # build only
    qmx m       # monitor only

Exit monitor with Ctrl+T then Ctrl+X.

## Layout (landscape 1280 × 720)

    +----------------------------------------------------------+
    | Top bar (60 px)  freq | mode | s-meter | menu            |
    +----------------------------------------------------------+
    | Spectrum (200 px) green line + amber center + cyan touch |
    |                   + horizontal dB grid + live dB labels  |
    +----------------------------------------------------------+
    | Label band (18 px) -24k -12k 0 +12k +24k + tick marks    |
    +----------------------------------------------------------+
    | Waterfall (412 px) 1280 × 412, newest row at top         |
    |                    classic SDR gradient                  |
    +----------------------------------------------------------+
    | Bottom bar (30 px) status, span, fps                     |
    +----------------------------------------------------------+

## Touch-to-tune

- Touch anywhere on spectrum or waterfall → cyan cursor tracks your finger
- Drag to position → cursor follows live
- Lift → CAT `FA` command sent with rounded 10 Hz target; QMX retunes; spectrum re-centers on the tapped signal
- Center cursor (amber, fixed at x=640) marks where the QMX is currently tuned

CAT writes are internally rate-limited to one per 200 ms; rapid taps within that window are dropped silently.

## Spectrum smoothing and autoscale (Phase 5.4)

The spectrum is smoothed per bin with an exponential moving average (α = 0.4) before display, balancing visual stability against the snappy response needed to see real signals (CW, SSB attack).

The dB display range is autoscaled once per second using the median of the spectrum (approximating the noise floor) and the maximum bin (the loudest signal). New range is `[median - 10 dB, max + 5 dB]`, clamped to a 40-120 dB span. Top-left and bottom-left labels on the spectrum show the current range and update with autoscale.

## Project layout

    qmx-panadapter/
    |-- main/
    |   |-- main.c                  app_main, orchestration
    |   |-- display/                LVGL bring-up via local BSP
    |   |-- ui/                     LVGL widgets, touch handler, canvases
    |   |-- cat/                    USB CDC-ACM + Kenwood CAT
    |   |-- audio/                  USB UAC + ring buffer
    |   |-- dsp/                    esp-dsp FFT, spectrum mutex
    |   `-- render/                 30 Hz render task, smoothing, autoscale
    |-- components/
    |   |-- m5stack_tab5/                  M5Stack local BSP (ST7123 panel + touch)
    |   `-- espressif__usb_host_uac/       Patched UAC component (see Quirks)
    `-- managed_components/         Other deps fetched via idf_component.yml

## Quirks and trade-offs (read this!)

A few decisions that matter and that someone (including future-me) will otherwise relearn the hard way.

### PI4IO expander must be initialized explicitly before display bring-up

The Tab5's PI4IO I/O expander holds `LCD_RST` and `TP_RST` low at chip power-on. The BSP's `bsp_display_start_with_config` does **not** call `bsp_io_expander_pi4ioe_init` internally. Without it, on a true cold boot the panel never comes out of reset, doesn't respond to DSI commands, and `esp_lcd_new_panel_io_dbi` hangs forever in the read FIFO wait loop.

Soft resets and USB-tethered development mask this entirely: the expander is a separate I²C peripheral that retains its state across ESP32 resets, so LCD_RST and TP_RST stay high from the previous boot.

Our `display_init` calls `bsp_i2c_init()` + `bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle())` at the very start, then waits 120 ms for both chips to come out of reset before letting the BSP probe I²C for the touch controller. Without that delay, the probe runs too fast on cold boot, falls back to ILI9881C panel, then mismatches with the (now-responding) ST7123 touch chip and asserts.

### Patched UAC component lives in `components/`

`espressif/usb_host_uac` was hand-patched in Phase 3.2 to set `create_background_task = true` in `uac_host_install`. Without this, UAC and CDC-ACM cannot coexist on the same USB host on this hardware.

Because the component manager refuses to clean `managed_components/` after a hand-patch, the component was moved permanently to `components/espressif__usb_host_uac/`. **Trade-off:** no auto-updates from the registry. If you want to update it, fetch a fresh copy, re-apply the patch, and replace the directory.

### LVGL software rotation costs ~50% FPS

The ST7123 panel is natively portrait 720×1280 and does *not* implement `esp_lcd_panel_swap_xy`. To get landscape, we use `bsp_display_cfg.sw_rotate=1` plus `lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)`.

LVGL rotates every flush in software (`rotate90_rgb565`). Real-world impact on this hardware: spectrum FPS drops from ~22 (portrait) to ~13 (landscape).

Acceptable for a panadapter — commercial radios in this class run 10–15 Hz waterfalls. If FPS ever feels insufficient, Phase 6.3 would render directly in panel-native orientation and bypass LVGL rotation entirely.

### IDLE-task watchdog is disabled

LVGL's rotation pipeline keeps CPU0 busy long enough that the IDLE0 task can't reset its watchdog within the default 5 s. The system isn't actually hung — it just doesn't yield to IDLE. We:

- Disabled `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/CPU1`
- Bumped `CONFIG_ESP_TASK_WDT_TIMEOUT_S` from 5 to 30 (safety net for genuinely stuck app tasks)

Real hangs in our app tasks will still be caught. Idle starvation under LVGL load won't generate noise.

### Engineering-sample silicon (ESP32-P4 v1.3)

- Use `CONFIG_ESP32P4_REV_MIN_0=y` (the build will fault `Illegal instruction` if revision is set too high)
- Cap CPU at 360 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=360`)
- Both already baked into `sdkconfig`

### Waterfall scroll trick

We allocate the waterfall canvas at 2× height (1280×824 RGB565). Each tick writes the new row at both `s_wf_head` and `s_wf_head + WATERFALL_H`, then decrements `s_wf_head` (with wrap). The canvas view pointer is moved through the buffer instead of `memmove`-ing. ~130 µs/tick instead of ~92 ms/tick.

## Tools

### `qmx` PowerShell helper

Add to your `$PROFILE`. Adjust the COM port and IDF path:

    function qmx {
        param([string]$cmd = "fm")
        if (-not $env:IDF_PATH) {
            & C:\esp\v5.4.4\esp-idf\export.ps1
        }
        switch ($cmd) {
            "b"   { idf.py build }
            "f"   { idf.py flash }
            "m"   { python -m esp_idf_monitor -p COM3 build\qmx_panadapter.elf }
            "fm"  { idf.py flash; python -m esp_idf_monitor -p COM3 build\qmx_panadapter.elf }
            "bfm" { idf.py build flash; python -m esp_idf_monitor -p COM3 build\qmx_panadapter.elf }
        }
    }

Exit monitor with `Ctrl+T` then `Ctrl+X` (works on Danish/non-US keyboard layouts where `Ctrl+]` is awkward).

## License

MIT (see LICENSE).