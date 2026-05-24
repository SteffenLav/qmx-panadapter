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

### Noise floor pumping on QMX I/Q (under investigation)

With the QMX in IQ Mode, the panadapter shows a slow ~13 s cycle on the displayed noise floor: ~6 s at a high level (~104 dB mean), then a 7 s descending ramp through middle values back to a ~60 dB floor, then a sharp jump back up. Strong tones (birdies, real signals) stay rock-steady through the cycle; only the broadband floor between them pumps.

What we have ruled out, with logs:
- The QMX itself. The same QMX into HDSDR on a PC, or the iPad SDR app, shows a flat steady noise floor with no pumping. The I/Q stream out of the QMX is clean.
- CAT polling. Pumping persists with the CAT poll task disabled and no touch events.
- Ring buffer overflow. `RX <n> pairs/s` stays rock-steady at 47880–48045 throughout. The only `DROPPED` warning fires once at USB connect; never during pumping.
- Ring buffer sizing. 64 KB ring = ~170 ms headroom at 48 kHz int16 stereo. Generous.
- Task placement / starvation. Audio on core 1 pri 5, FFT on core 1 pri 4, render on core 0 pri 3, UAC background on core 0 pri 5. Moving audio_task to core 0 at pri 6 did not change the pumping.
- Our FFT, window, and dB conversion math. Same-frame diagnostics (PRE-FFT workbuf, POST-FFT workbuf, samples, bin dB) all line up with expected behavior given the input.

What we observed but cannot yet explain:
- During "high" phase, `samples[0..5]` reads as 0 while `samples[1020..1023]` reads as full-scale signal. A step-like pattern within the 21 ms FFT window. The FFT of such a step legitimately spreads broadband energy across all bins, which matches the high-mean reading.
- The raw bytes coming in from the UAC layer during the same instant do contain real non-zero data. The zeros appear somewhere between `raw[]` and the consumer side of the ring, but neither the decoder nor the ring buffer code shows an obvious mechanism.

Likely remaining root causes (in rough order):
1. A subtle interaction in the patched `usb_host_uac` component when it coexists with CDC-ACM — possibly periodic isochronous-transfer replay or buffer reuse that escapes the pairs/s counter.
2. A cache coherency or DMA-vs-CPU race we cannot see through `printf` debugging.
3. Something less likely in the consumer / ring / DSP path that we have not yet thought to check.

Pinning this further likely needs a USB protocol analyzer or a logic-analyzer trace on the ESP32-P4 isoc transfers, both of which are out of scope for now. The artifact is cosmetic — real signals remain visible and stable — and we are shipping with it documented. The planned DC blocker on the I/Q stream (see Phase 5.6 in the roadmap and the DC-spike suppression entry below) is unlikely to mask this fully but is standard SDR hygiene and may reduce the visual impact.

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
- **DC-spike suppression.** Mask the center 3 FFT bins for v1; later, add a time-domain running-mean DC block on the I/Q stream before the FFT. See [`docs/panadapter-display-design.md`](docs/panadapter-display-design.md) for background.
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