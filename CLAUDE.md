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
  ui/ft8_screen_view.c    FT8 RX decode list UI, touch-drag row selection, TX controls
  ui/ft8_tx_modal.c       TX confirmation modal (message preview, countdown, arm/cancel)
  cat/cat.c               USB CDC-ACM + Kenwood CAT (FA/MD/FW poll, tune write)
  audio/audio.c           USB UAC + ring buffer producer (core 0, polling)
  dsp/dsp.c               FFT consumer (reads ring buffer), spectrum mutex, DC blocker
  dsp/iq_balance.c        Blind adaptive Gram-Schmidt I/Q correction (Phases A–C)
  ft8_tx.c                FT8 TX engine: build/arm/run/abort, ft8_find_clear_tone_hz()
  ft8_test.c              FT8 slot loop: RX decode or TX burst each 15-second slot
  ft8_qso.c               Auto search-and-pounce QSO state machine (WAIT_RPT/RR73/DONE)
  ft8_status.c            Mutex-protected status string written by ft8_task, read by LVGL timer
  render/render.c         30 Hz render task, EMA smoothing, dB scaling
  render/render_waterfall.c  Waterfall tick, double-height canvas scroll trick
  screenshot/screenshot.c UART screenshot dump (hidden long-press, top-left 80×80)
  util/fps.c              FPS counter
```

Data flow: **audio → ring buffer → dsp (FFT) → spectrum mutex → render → LVGL canvases**

FT8 TX data flow: **ft8_screen_view (tap) → ft8_tx_modal (confirm) → ft8_tx_arm() → ft8_test slot loop → ft8_tx_run() → CAT TA; burst**

## Critical quirks

### PI4IO expander must be initialized before display bring-up
`display_init` calls `bsp_i2c_init()` + `bsp_io_expander_pi4ioe_init()` first, then waits 120 ms. Skipping this causes a cold-boot hang in the DSI FIFO loop. Soft resets mask the bug because the expander retains state across ESP32 resets.

### Patched components in `components/`
`components/espressif__usb_host_uac/` is a hand-patched fork with `create_background_task = true`. Required for UAC + CDC-ACM to coexist on the same USB host. Do not replace with the registry version without re-applying the patch.

`components/espressif__esp_lcd_touch_st7123/` is a hand-patched fork fixing ST7121 compatibility (see **ST7121 has an incomplete register map** below). Do not replace with the registry version.

### LVGL software rotation (~50% FPS cost)
The display panel is natively portrait; landscape is achieved via `lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)`. Every LVGL flush goes through `rotate90_rgb565`. FPS is ~13 landscape vs ~22 portrait. Acceptable for a panadapter.

**Do not enable `CONFIG_LVGL_PORT_ENABLE_PPA=y`.** The PPA driver and the USB host stack (UAC + CDC-ACM) compete for DW-GDMA channels. Enabling PPA silently kills QMX connectivity — audio and CAT both stop. Tested and confirmed broken.

Phase 6.3 (FPS recovery) requires a full native-portrait UI rewrite: LVGL configured as 720×1280, all widget positions transposed (landscape x↔y swap), all canvas drawing code rewritten for portrait orientation. Significant work; not yet done.

### Hardware revision detection — ST7121 vs ST7123 touch controller
Newer Tab5 units ship with an **ST7121** touch controller (I2C 0x55, FW version = 1) instead of the original **ST7123** (same address, FW version = 3). Both also differ from the older ST7703/GT911 hardware.

Detection lives in `bsp_detect_display_type()` in `components/m5stack_tab5/m5stack_tab5.c`. It probes I2C on startup, reads register 0x0000 from 0x55, and returns `BSP_DISPLAY_TYPE_ST7121` or `BSP_DISPLAY_TYPE_ST7123` accordingly. The result is cached in `s_detected_display_type` — do not call the function more than once per boot (the second call used to re-probe the touch chip after the 800 ms DISPON delay, leaving the bus dirty).

**I2C speed for ST7121/ST7123 touch must be 100 kHz.** The detection probe uses 100 kHz; `bsp_display_indev_init_to_st7123()` must also use 100 kHz (`tp_io_config.scl_speed_hz = 100000`). Do not change this to `CONFIG_BSP_I2C_CLK_SPEED_HZ` (400 kHz) — ST7121 does not respond reliably at 400 kHz.

**ST7121 has an incomplete register map.** The `read_fw_info()` function in `esp_lcd_touch_st7123.c` originally called `ESP_RETURN_ON_ERROR` for all three register reads (`FW_REVISION_REG 0x000C`, `MAX_X_COORD_H_REG 0x0005`). ST7121 NACKs both — this alone would cause `esp_lcd_touch_new_i2c_st7123()` to return an error, `bsp_display_start_with_config()` to return NULL, `ESP_ERROR_CHECK(display_init())` to abort, and an immediate panic → reboot **before any UI renders**. v0.13.1 forked the component to `components/espressif__esp_lcd_touch_st7123/`, making only `FW_VERSION_REG (0x0000)` mandatory (others LOGW on failure) and clamping `max_touches > 10` in `read_data()` to prevent a stack smash.

Both ST7121 and ST7123 use the same touch driver (`esp_lcd_touch_new_i2c_st7123`) and the same init path (`bsp_display_indev_init_to_st7123`). The ST7121 and ST7123 LCD panels use separate init sequences (`bsp_display_new_with_handles_to_st7121` / `_to_st7123`) with different DSI lane bitrates (1300 Mbps vs 965 Mbps).

### New-Tab5 boot-loop — RESOLVED (v0.15.0): it was WiFi, not touch
The endless reboot reported on new ST7121 units was **never a display/touch problem**. The GUI rendered fully, then the device reset ~2.5 s later. v0.12.1 (I2C speed) and v0.13.1 (touch register-map tolerance) were shots in the dark without a serial log and fixed nothing — that unit's ST7121 reads every register fine.

The serial log (captured via `tools/capture_serial_log.ps1` — see below) showed the real crash: `assert failed: netif_add ... netif.c:420 (netif already added)`, firing in the background `wifi_task` after `app_main` returns. Root cause: on newer ESP-Hosted/C6 firmware the hosted layer **auto-creates** the default `WIFI_STA_DEF` netif and registers the default STA handlers once the C6 link comes up; `wifi_task` then called `esp_netif_create_default_wifi_sta()` again, registering a **second** copy of `wifi_default_action_sta_start`. One `WIFI_EVENT_STA_START` ran the start action twice, and `esp_netif_start_api()` has no double-add guard → second `netif_add()` → `LWIP_ASSERT` → panic loop. Older C6 firmware doesn't auto-create, so the dev bench (single registration) never reproduced it.

Fixed in `main/wifi/wifi.c`: (1) only call `esp_netif_create_default_wifi_sta()` if `WIFI_STA_DEF` doesn't already exist; (2) don't call `esp_wifi_start()` with no SSID — STA_START is never raised, so a no-credentials unit cannot hit the path regardless of C6 timing (`panadapter_wifi_reconnect()` starts the radio when credentials are first saved).

**Diagnostic tooling** (kept for future remote debugging, attached to releases):
- `tools/capture_serial_log.ps1` — no-install PowerShell script (.NET `SerialPort`), logs USB console output across reboot cycles to a timestamped `.txt`, with auto-reconnect. Deliberately does **not** set `DtrEnable`/`RtsEnable` — the ESP32-P4 USB-Serial/JTAG auto-reset-to-bootloader circuit watches those lines; asserting them can drop the chip into the ROM download stub with no output.
- `docs/serial-log-howto.md` / `.pdf` — end-user step-by-step guide.
- Decode a panic backtrace with `riscv32-esp-elf-addr2line -e build/qmx_panadapter.elf <addr> ...` — but note `build/` must match the flashed commit (the embedded SHA differs only by compile timestamp; code addresses match if same commit + same pinned IDF).

The v0.13.1 touch-driver fork (register-map tolerance, `max_touches` clamp) is harmless and kept as defensive cover for ST7121 units that *do* have an incomplete register map.

### IDLE watchdog disabled
`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/CPU1` are off. The LVGL rotation pipeline keeps CPU0 busy past the default watchdog window. App-task watchdog (30 s) is still active.

### Audio task is polling, not event-driven
`audio_task` runs on core 0 with `uac_host_device_read` + a drain loop. Event-driven reads caused noise-floor pumping (slow ~13 s cycle) due to truncated UAC chunks saturating the FFT input. Do not revert to event-driven.

### Waterfall double-height buffer
`1280 × 824` RGB565 canvas (2× waterfall height). Each tick writes the new row at `s_wf_head` and `s_wf_head + WATERFALL_H`, then moves the view pointer. Avoids `memmove` (~130 µs vs ~92 ms per tick).

### 12 kHz IF offset
The QMX presents IQ with +12 kHz IF offset — the VFO signal lands at +12 kHz in baseband. Spectrum and waterfall shift bin selection by `n_bins/4` to center the VFO signal visually. Touch-to-tune math uses the raw CAT frequency, so no adjustment needed there.

### FT8 capture window must be UTC-boundary-capped, not fixed-sample-count (v0.15.1)
`dsp_ft8_capture()` used to wait for a fixed 180000 samples (nominally 15.000 s @ 12 kHz). The QMX's USB audio clock isn't bit-exact 48 kHz, so 180000 samples actually take a hair over 15.000 s wall-clock — the capture window slides later by ~0.2–0.4 s every slot. After ~12–15 slots (~3 min) the FT8 signal falls outside ft8_lib's decoder time-search window: candidates stay high (~140) but decodes drop to 0. A mode-bounce (FT8 → Panadapter → FT8) "heals" it by resetting to a fresh UTC boundary — that was the tell that pinned this down.

Fix: `ft8_task` (`ft8_test.c`) computes `ms_to_boundary = 15000 - start_off_ms` and passes that as the capture timeout (clamped to `[2000, SLOT_TIMEOUT_MS]`). `dsp_ft8_capture()` treats hitting that boundary as the normal case — it zero-pads any sample shortfall (dead air after the FT8 signal) instead of returning `ESP_ERR_TIMEOUT`. The per-slot log gained `off=%+dms` (capture-start offset from the UTC boundary): should stay near 0 forever; if it climbs again, the anchor is broken.

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

## I/Q balance correction

`dsp/iq_balance.c` — blind adaptive Gram-Schmidt orthogonaliser applied per sample in `audio.c` before samples enter the ring buffer.

- **DC removal**: exponential tracker, τ = 1 s
- **Power tracking** (I² and Q²): τ = 200 ms
- **Cross-product tracking** (I·Q): τ = 1 s
- **Two-speed startup** (Phase C): all alphas run at 8× for the first 2 s of signal after each reset, giving ~125 ms convergence; then drop to steady-state for stable long-term tracking
- **Correction**: `q_out = (q - K_phi·i) × K_amp`; I channel is unchanged
- **Toggle**: `iq_balance_set_enabled()` / `iq_balance_is_enabled()` — wired to the settings drawer switch (Phase B); re-enabling calls `iq_balance_reset()` so the estimator reconverges from clean state

Do not call `AI1;` on the QMX CAT port — it partially executes (enables auto-info mode) despite returning `?;`, which breaks FA polling for the entire session until power cycle.

## FT8 TX (v0.12.0) — key design points

`FT8_TX_SEND_LIVE = 1` in `ft8_tx.c` — this is live TX; the radio keys up for real.

- **CAT burst sequence**: `TX;` → 79× `TA<freq>;` at 160 ms cadence (absolute `esp_timer_get_time()` targets, no drift) → `TA0;` → 5 ms settle → `RX;`. Always runs the tail even on abort or error.
- **Tone spacing**: 6.25 Hz per FT8 tone index (0–7). `freq = base_hz + tone * 6.25f`.
- **Slot parity**: `((unix_sec / 15) % 2) == 0` → EVEN. Reply fires on the opposite parity from the heard slot. CQ fires on any slot unless `use_parity=true` + `want_even_slot` set.
- **`cat_poll_set_paused(true/false)`** in `cat.c` — cooperative flag; TX burst holds this for its entire duration so the poll task doesn't interleave commands. Do **not** use `vTaskSuspend` — that can deadlock the CDC-ACM driver mutex.
- **Digi-mode pre-flight**: checked at arm time (blocking, ~1 s worst case); re-checked at burst time (cached string read, free). If mode has drifted at burst time, abort cleanly before `TX;` — never attempt a corrective switch at burst time (would desync slot start).
- **`ft8_find_clear_tone_hz()`**: heap-allocs a snapshot of the heard-station table, builds a `uint64_t` bitmask of occupied 50 Hz bins (200–2800 Hz, 52 slots), walks outward from bin 26 (1500 Hz) with ±1 guard, returns first clear bin in Hz. Returns `FT8_TX_CQ_DEFAULT_FREQ_HZ` (1500) on OOM or empty table.

### Touch-drag row selection (ft8_screen_view.c)

- `ROW_HOLD_SELECT_MS = 400` — finger must be held ≥ 400 ms before selection mode activates.
- On threshold crossing: `lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE)` locks list scroll so drag moves the highlight rather than scrolling.
- On `RELEASED` or `PRESS_LOST`: `lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE)` restores scroll unconditionally.
- `screen_y_to_row(abs_y)`: maps absolute screen Y to row index accounting for `s_list` coords and `lv_obj_get_scroll_y()`.

## FT8 QSO state machine — pounce + CQ-run (v0.13.0 / v0.15.0)

`ft8_qso.c` — one state machine, two roles. Driven by three call sites:
- `ft8_qso_start(tx1_req)` — LVGL task: pounce. Accepts a pre-built TX1 (`<them> <me> <grid>`), enters WAIT_RPT.
- `ft8_qso_start_cq(cq_req)` — LVGL task: CQ-run. Arms CQ, enters CQ.
- `ft8_qso_advance(slot_sec)` — **decode task** after each RX slot: scans `ft8_screen` for messages with `last_utc == slot_sec`, decides the next message.
- `ft8_qso_on_tx_complete()` — **capture task** right after each burst: re-arms the current outgoing message.

**Two role flows** (third field of the FT8 message decides):
```
POUNCE (we answered their CQ)          CQ-RUN (they answered our CQ)
 TX1 <them> <me> <grid>                 CQ  CQ <me> <grid>
 RX  <me> <them> <report>   WAIT_RPT     RX  <me> <them> <grid|rpt>   CQ
 TX2 <them> <me> R<report>               TX  <them> <me> <report>     (snr proxy)
 RX  <me> <them> RR73/73    WAIT_RR73    RX  <me> <them> R<report>    WAIT_ROGER
 TX3 <them> <me> 73                      TX  <them> <me> RR73
                            WAIT_DONE → (final fired, ft8_tx IDLE) → DONE → IDLE
```

**Patience / retry (v0.15.0)**: the *current outgoing message* (`s_cur_req`) is re-armed every TX slot by `on_tx_complete()` (CQ cadence + exchange retries), so we keep pushing the same call until progress. `QSO_TIMEOUT_SLOTS = 4` consecutive RX slots with no progress → give up. CQ-originated QSOs **resume CQ** on timeout (don't drop the frequency); pounce QSOs go sticky TIMEOUT.

**Deferred arming (v0.15.0)**: `advance()` runs in the decode task ~4 s into the *next* slot — usually while our re-armed burst is already ACTIVE, when `ft8_tx_arm()` refuses. So advance() only updates state + `s_cur_req`; `rearm_current()` (from `on_tx_complete()`, or `arm_current_if_idle()` as a safety net) does the actual arm. WAIT_DONE arms the final exactly once then clears `s_have_cur`. This fixes the v0.14.0 bug where a CQ reply detected during the next CQ burst could never transition out of CQ.

**Report value**: CQ-run sends a signal report built from the answering station's coarse proxy SNR (`fmt_report`, clamped −24..+15, e.g. `-07`). Not WSJT-X-calibrated.

**CQ-row filtering**: `ft8_qso_cq_filter_active()` is true throughout a CQ-originated session; `rebuild_list()` in `ft8_screen_view.c` then hides other stations' `CQ ` rows so replies to us stand out.

**Decode-list aging (v0.15.1)**: the list is a live picture of who's on frequency *now*, not a history log. `ft8_screen_get_all()` in `ft8_screen.c` expires entries not re-decoded within `FT8_ROW_STALE_SEC` (60 s) during the snapshot. The 1 Hz clock timer (`t_clock_cb`) sets `s_refresh_pending = true` every tick so stale rows drop even when the band is quiet and nothing new is decoded. "Heard: N" → "Active: N".

**No slot-skip**: `ft8_task` captures *every* non-TX slot, including the parity opposite an armed TX. With ping-pong decode a capture is exactly one slot (15 s) and ends on the next boundary, so the armed burst still fires on time — and capturing the opposite slot is the only way to hear the station we're working. (The old v0.13.1 parity-skip was removed in v0.15.0: it made us deaf on the partner's slots. The "~19 s swallow" it guarded against was pre-ping-pong, when capture+decode were synchronous.)

**UI**: TX confirmation modal has "Auto Pounce" button (visible for REPLY kind only) alongside "Transmit". The left-pane status label (`s_lbl_tx`) is always visible and shows — in priority order: ACTIVE (red), ARMED (amber), QSO complete (green), QSO timeout (orange, tap to clear), `ft8_status` passthrough (dim white).

**`ft8_status.c`**: tiny mutex-protected string written by ft8_task (RX capturing, decoding, TX symbol count), read by the 1 Hz LVGL timer. Shows what the FT8 process is doing at all times.

**`ft8_tx` extensions**: added `FT8_TX_KIND_ROGER_RPT` / `FT8_TX_KIND_73` and `extra_field[8]` to `ft8_tx_request_t`. `ft8_tx_build_request` now takes a `const char *extra` parameter (NULL = use `my_grid`, for backward compat).

## Branch state

| Branch | What | State |
|--------|------|-------|
| `main` | v0.15.2 — 40-row FT8 decode list (LV_MEM_SIZE_KILOBYTES 256), display brightness slider (settings drawer), persistent UI mode (Panadapter/FT8 survives reboot via `ui_apply_saved_mode()`) | stable on ST7123 and ST7121 |

## Next up (v0.16.0)

ADIF logging: write each completed QSO to an ADIF file on-device; log view in FT8 screen; upload via web UI to LOTW/QRZ/eQSL/POTA.app.
