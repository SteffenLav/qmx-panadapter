# QMX+ Panadapter

*By Steffen Lav (OZ1LAV).*

A standalone real-time panadapter — spectrum analyzer and waterfall — for the [QRP Labs QMX/QMX+](https://www.qrp-labs.com/qmxp.html) HF transceiver, running on the [M5Stack Tab5](https://docs.m5stack.com/en/core/tab5) (ESP32-P4 with a 5" 720×1280 touch display).

The QMX exposes I/Q audio over USB UAC plus CAT control over USB CDC-ACM. The Tab5 connects to the QMX as a USB host, decodes the I/Q in real time on the ESP32-P4, and renders a touch-driven panadapter with tap-to-tune.

![Panadapter display — QMX+ tuned to 14.000 MHz with CW activity](docs/panadapter-mockup-ideal.svg)

*The display in action: 48 kHz of spectrum centered on the QMX VFO, live FFT trace on top, scrolling waterfall below, magenta VFO marker with CW filter passband shading. See [`docs/panadapter-display-design.md`](docs/panadapter-display-design.md) for the design notes including the hardware artifacts (DC spike, I/Q image) you'll see in practice.*

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
| 5.4   | EMA spectrum smoothing + autoscaling dB range (superseded by 5.5) | done |
| 5.5   | Static dB range (manual Ref/Range convention), correct 24-bit scaling | done |
| 5.6   | One-pole IIR DC blocker on the I/Q stream before FFT | done |
| 5.7   | Polling audio_task on core 0 + drain loop — fixes noise-floor pumping | done |
| 5.8   | dBm calibration scale anchored on QMX dummy load | done |
| 5.9   | Larger fonts + continuous green spectrum curve + dim fill | done |
| 5.10A | CAT mode polling (alternating FA / MD; 5 Hz each) | done |
| 5.10B | Band derivation from QMX frequency | done |
| 5.10C | Real frequency axis labels (absolute MHz centered on VFO) | done |
| 5.10D | Top-bar layout, S-meter, settings drawer with presets and sliders | done |
| 5.10E | 12 kHz IF offset compensation (signals centered on VFO) | done |
| 5.10F | Waterfall auto-floor tracking + mode-aware tune snap | done |
| 5.10G | Passband indicator from CAT FW (with mode defaults) | done |
| 5.10H | Faster CAT poll + optimistic touch-to-tune UI | done |
| 5.10I | Bigger burger and close buttons (80x80) | done |
| 6.1   | Touch-to-tune via CAT FA, live cyan drag cursor | done |
| 6.2   | Landscape rotation 1280×720 (LVGL software rotation) | done |
| —     | Cold-boot fix (PI4IO expander init for LCD_RST / TP_RST) | done |

See the [Roadmap](#roadmap) at the bottom for what's next.

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
- Lift -> CAT `FA` command sent; QMX retunes; spectrum re-centers on the tapped signal. Rounding is mode-aware (Phase 5.10F): USB/LSB snap to 500 Hz, CW to 10 Hz, FT8/digi to 100 Hz, AM/FM to 1 kHz
- Center cursor (amber, fixed at x=640) marks where the QMX is currently tuned

CAT writes are internally rate-limited to one per 200 ms; rapid taps within that window are dropped silently.



## Top bar, S-meter, settings drawer (Phase 5.10)

The top status bar reads `Band | Mode | Center Freq | Signal` left to right, with the frequency centered. Band and mode come from CAT (FA / MD / FW round-robin poll at 50 ms intervals = each field refreshes every ~150 ms). The band name is derived from the VFO frequency using widened ranges that cover the QMX's tunable region beyond the strict IARU edges. The Signal field shows S-units derived from `dsp_get_peak_dbm_around_vfo(64, ...)` (the peak dBm within +/-64 bins, about +/-3 kHz of the VFO) sampled at 5 Hz in the render task.

Under the spectrum, the frequency axis labels show absolute MHz centered on the VFO (e.g. `13.988 / 13.994 / 14.000 / 14.006 / 14.012` at 48 kHz span), refreshed on every CAT freq update.

**IF offset compensation (Phase 5.10E).** The QMX presents I/Q with a +12 kHz IF offset: the signal at the QMX dial frequency lands at +12 kHz in the baseband. Both `ui_push_spectrum` and `render_waterfall_tick` shift the displayed bin selection right by n_bins/4 so the tuned signal appears at the visual center under the amber VFO marker. Touch-to-tune math is unchanged because `s_last_qmx_freq_hz` is the dial reading, not the LO.

**Waterfall auto-floor (Phase 5.10F).** The waterfall's "darkest color" level is no longer fixed; it tracks the running median of the spectrum once per second (EMA-smoothed, alpha 0.3, clamped -150 to -30 dBm). The dark background therefore follows actual band conditions instead of a hard-coded -130 dBm anchor. The spectrum trace stays user-controlled via the settings drawer (manual ref/range like commercial SDRs).

**Mode-aware tune snap (Phase 5.10F).** Touch-to-tune rounding is mode-dependent: USB / LSB snap to 500 Hz (voice channel grid), CW to 10 Hz (precision), FT8 / digi / RTTY / FSK to 100 Hz, AM / FM to 1 kHz. The current mode is cached from CAT MD into `s_current_mode`.

**Passband indicator (Phase 5.10G).** Two 2-px-wide medium-grey vertical lines mark the QMX receiver's current passband edges. Width comes from CAT FW polling (the QMX returns real values: 300 Hz for CW, 2500 Hz for USB/LSB, 3200 Hz for FSK). Falls back to per-mode defaults if FW reports nothing. Passband geometry is mode-aware: CW/AM/FM are symmetric around the VFO, USB extends from VFO+200 Hz to VFO+200+width, LSB is the mirror, FT8/digi follows USB.

**Faster CAT + optimistic UI (Phase 5.10H).** CAT poll interval dropped from 200 ms to 50 ms so dial-spinning no longer skips. Touch-to-tune optimistically updates the on-screen frequency label immediately on a successful CAT write rather than waiting ~150 ms for the next FA poll to confirm.

**Settings drawer (Phase 5.10D).** The burger button on the top right opens a 400 px right-side settings drawer with a 250 ms slide-in animation. Contents:
- **Presets** (HF Normal / HF DX / Strong Sig.) -- each sets dB range and EMA smoothing in one tap
- **dB Range sliders** (Min and Max in dBm) -- live updates `ui_set_db_range()`
- **Smoothing slider** (EMA alpha, 0.05 to 1.00) -- live updates `render_set_ema_alpha()` (the formerly hard-coded `SMOOTH_ALPHA` is now a runtime variable)

**Bigger touch targets (Phase 5.10I).** The burger button is 80x80, parented to the screen so it overflows downward into the spectrum area without being clipped by the top bar. The drawer close X is also 80x80. Both icons use Montserrat 32 to fill the button size cleanly. A 200x120 deadzone in the top-right of the spectrum suppresses tunes that overlap the burger button.

## Spectrum smoothing, dB range, dBm calibration (Phase 5.5 / 5.7 / 5.8)

The spectrum is smoothed per bin with an exponential moving average (α = 0.4) before display, balancing visual stability against the snappy response needed to see real signals (CW, SSB attack).

The displayed dB range is fixed at -130 to -30 dBm, matching commercial SDR convention (HDSDR, SDR Console, Flex Maestro) where the user picks a manual Ref/Range rather than letting the display continuously rescale. Continuous autoscale was tried in Phase 5.4 and removed in 5.5 — it actively hid signal-vs-noise dynamics. The -130 dBm floor and -30 dBm ceiling cover the real signal range from quiet HF noise floor (anchored on dummy load in Phase 5.8) to S9+40 strong signals.

**dBm calibration (Phase 5.8).** The displayed dB values are calibrated against the QMX on a dummy load: with no antenna, the median dB across all bins per second is logged, and `DSP_DB_CALIBRATION_OFFSET` is set to `-130 - median` so the noise floor reads -130 dBm. On this hardware the measured offset is -148.0 dB. S-unit reference: S9 = -73 dBm, S5 ≈ -97 dBm, S1 ≈ -121 dBm. Signals stronger than ~S9+40 (-30 dBm) clip at the display ceiling but still indicate presence at full-scale.

**Continuous green curve (Phase 5.9).** Each column's trace is connected to the previous column's `y_top` with a bright vertical line, so the spectrum reads as a continuous curve rather than disconnected per-column dots. Below the curve, a dim ~25% green fill (RGB565 `0x01C0`) gives the look of [docs/panadapter-mockup-ideal.svg](docs/panadapter-mockup-ideal.svg). Grid lines at -120, -100, -80, -60, -40 dBm.

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
    |-- docs/
    |   |-- architecture.md                Overall signal path
    |   `-- panadapter-display-design.md   Display mockups + DC-spike / I/Q image notes
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

### Noise floor pumping on QMX I/Q (resolved in Phase 5.7)

Earlier versions of the panadapter showed a slow ~13 s cycle on the displayed noise floor: ~6 s at a high level (~104 dB mean), then a 7 s descending ramp back to a low floor, then a sharp jump back up. The same QMX into HDSDR or an iPad SDR app showed a flat steady noise floor, so the I/Q stream out of the QMX was always clean.

**Root cause:** data starvation in our event-driven `audio_task`. Non-blocking `uac_host_device_read` calls on core 1 periodically returned truncated chunks. The decoder's `peak L/R` saturated at 16384 every period (an artifact of the broken pipeline, not real signal amplitude), and the FFT input acquired periodic step discontinuities which spread broadband energy across all bins on a slow envelope.

**Fix (commit `6d3d968`):** replicate qrp_companion's `mic_task` producer-side architecture:
- Polling loop on core 0 priority 3 (was event-driven on core 1 priority 5)
- `uac_host_device_read` with 25 ms timeout
- Drain loop inside `process_rx` reads until the UAC driver returns 0 bytes
- `RX_BUF_BYTES` raised from 4096 to 19200 to match the UAC internal buffer
- 24-bit → 16-bit scaling restored to `>> 8` (the Phase 5.5 `>> 9` was a misdiagnosis)
- Display dB range shifted to 10–130 dB to cover real signal amplitudes (later recalibrated to -130 to -30 dBm in Phase 5.8)

Inspired by [`qrp_companion`](https://groups.io/g/QRPLabs/topic/118645485) by Zhenxing Han (N6HAN), who confirmed his code on Tab5 did not exhibit the issue and offered his source as reference. Thanks Zhenxing.
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

---

## Roadmap

### Next up

Concrete items planned for the near term, in roughly the order they'll likely be tackled.

- **Phase 6.3 — Native-orientation rendering.** Render directly in the panel's native portrait coordinates and skip LVGL's software rotation, recovering the ~50% FPS lost to `rotate90_rgb565`. The display still appears landscape because the device is held that way; only the pixel layout changes.
- **NVS settings persistence.** Survive power cycles for user preferences: last VFO, autoscale state, EMA α, waterfall colour map. Foundation for everything else in this list.
- **Memory channels.** Quick-recall frequency presets — touch a slot, QMX retunes via CAT. Stored in NVS.
- **I/Q balance correction.** Per-band amplitude/phase coefficients applied before the FFT to push image rejection from the native ~30 dB to 50+ dB. One-time calibration step per band.

### Longer term

Ideas that fit the project but aren't on the immediate path. Order is rough; appetite and curiosity will decide.

- **FT8 decoder onboard.** Integrate [`ft8_lib`](https://github.com/kgoba/ft8_lib) using the existing audio pipeline (decimated to 12 kHz IF inside the QMX, or done locally from I/Q). Show decoded callsigns/grids overlaid on the spectrum at their carrier frequencies — a feature no commercial standalone panadapter currently offers.
- **CW decoder.** A Goertzel-based decoder on the demodulated CW passband, with text scrolling under the spectrum. The QMX itself already does this internally; question is whether to mirror its output via CAT or run a parallel decoder on the Tab5.
- **WiFi station mode + web UI.** ESP32-P4 has WiFi via the C6 co-processor. A small web server (Mongoose or `esp_http_server`) could expose the spectrum as a remote panadapter, with the Tab5 acting as a head unit at the rig.
- **Network CAT bridge.** TCP server forwarding CAT to the QMX, so WSJT-X / fldigi / N1MM on a PC can talk to the radio through the Tab5 — useful when the QMX is in the shack and the operating position is elsewhere.
- **QMX (small) support.** Same UI, different USB endpoint config and band table. Should be mostly a build-flag matter; the 5-band QMX speaks the same CAT and UAC.
- **Extended waterfall history.** PSRAM has plenty of room for several minutes of scrollback. Two-finger drag to scrub through history would be a natural UX fit.
- **Touch-to-tune refinements.** Pinch-to-zoom span (sub-48 kHz windows), drag-to-pan inside the current 48 kHz, snap-to-strongest-bin, configurable cursor offset for CW tone preference.
- **DSP polish.** Noise reduction, auto-notch, denoise — the feature surface the QuantumSDR Spectrum DSP M4 defines as the boutique-standalone target.

---

## License

MIT (see LICENSE). Copyright (c) 2026 Steffen Lav (OZ1LAV).