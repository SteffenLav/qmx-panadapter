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

**Claude Code note**: each PowerShell tool call is a fresh non-interactive shell — the user's `$PROFILE` (which defines `qmx`/`idfenv`) is not loaded, and `idf.py` fails with "IDF_PATH environment variable needs to be set". Activate the IDF environment in the same command chain first:
```powershell
& "C:\esp\v5.4.4\esp-idf\export.ps1" | Out-Null; idf.py build
```

**Build/flash/monitor — just do it**: after making code changes, build and flash (and monitor if useful) without asking for permission first, unless the user has explicitly said not to. Don't ask "want me to flash now?" — just run it.

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
  rtc/rtc.c               Thin driver for RX8130CE supercap RTC (I2C 0x32, 30-40 h backup)
  time_sync/time_sync.c   Global time-sync orchestrator: RTC → SNTP → QMX priority; periodic QMX poll task
  render/render.c         30 Hz render task, EMA smoothing, dB scaling
  render/render_waterfall.c  Waterfall tick, double-height canvas scroll trick
  screenshot/screenshot.c RGB565 framebuffer capture for the web UI's /ss.bmp endpoint
  util/fps.c              FPS counter
  util/diag_log.c         Opt-in ESP_LOG ring-buffer capture (web /api/log + serial)
  storage/settings.c      NVS-backed settings (debounced flush task)
  wifi/wifi.c             C6 co-processor WiFi bring-up + SNTP, QMX RTC push
  net/webserver.c         HTTP server: /, /api/status, /api/cmd, /ss.bmp, /api/log
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

### QMX power cycle doesn't always re-enumerate USB (v0.15.5)
With the USB cable left connected, power-cycling the QMX often does not trigger `AE_DISCONNECTED`/`AE_RX_CONNECTED` — the UAC device stays open, audio just goes silent for a few seconds then resumes. `process_rx()` in `audio.c` detects this directly: any poll with zero bytes/pairs sets `s_flat_reset_pending`, and the next poll with real samples calls `ui_flat_mode_reset()`. This is what re-seeds the flat-spectrum floor after a power cycle — the old approach (resetting on `AE_RX_CONNECTED` / CAT reconnect) never fired because no reconnect event occurs.

### Waterfall double-height buffer
`1280 × 824` RGB565 canvas (2× waterfall height). Each tick writes the new row at `s_wf_head` and `s_wf_head + WATERFALL_H`, then moves the view pointer. Avoids `memmove` (~130 µs vs ~92 ms per tick).

### 12 kHz IF offset
The QMX presents IQ with +12 kHz IF offset — the VFO signal lands at +12 kHz in baseband. Spectrum and waterfall shift bin selection by `n_bins/4` to center the VFO signal visually. Touch-to-tune math uses the raw CAT frequency, so no adjustment needed there.

`dsp_get_peak_dbm_around_vfo()` (used for the S-meter and the web UI's `signal_dbm`) must be centered on this same IF-shifted bin (`ui_get_if_bin_shift(DSP_FFT_SIZE)`), not on raw bin 0 (DC). Before v0.15.5 it searched around DC, which is dominated by constant DC/LO leakage — the S-meter read a near-constant S6 regardless of actual signal.

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

### QMX firmware version via `VN;`
`VN;` returns `VN<version>QMX;` (e.g. `VN1_03_002QMX;`) — the QMX/QDX firmware version, same string as the firmware filename without the dot. Queried once at CAT link-up (`process_cat_message` parses it into `s_qmx_fw`; `cat_get_qmx_fw()` exposes it). Surfaced in the boot/diagnostic log (`QMX firmware: ...`), the diag-log enable header (`qmx_fw=...`), and the web `/api/status` JSON (`qmx_fw`). Note `ID;` returns only the emulated Kenwood model (`ID020;` = TS-480), **not** the QMX firmware — use `VN;` for that. Confirmed on hardware (dev bench QMX is on `1_03_002`).

### Cross-thread CAT writes must go through the poll task, not the LVGL thread
The poll task is the only thing that should write to the CDC pipe at runtime. Writing a CAT/MM command directly from the LVGL/UI thread (e.g. a touch handler) races the `FA;`/`MD;`/`FW;` poll on the same pipe; the two commands interleave and the QMX gets a garble and returns `?;`, so the write lands only intermittently. Pattern: stash the request in a `volatile` and let `poll_task` drain it on its next cycle (see `s_pending_ssb_bw` / `cat_request_ssb_bandwidth()` in `cat.c`). FT8 TX uses the other valid approach — `cat_poll_set_paused(true)` for the burst's whole duration.

### SSB filter bandwidth needs THREE coordinated writes (the hard-won recipe)
Setting the QMX SSB RX filter (2500/2700/2900/3200 Hz) from the panadapter took a long debug to pin down. The QMX exposes the SSB filter as **two separate Menu-Manager items** plus a Kenwood read, none of which is sufficient alone:

- **`MMSSB|Filter RX=<hz>;`** — the *committed* value. Persists, shows in the QMX SSB menu (alongside `Filter TX`), but on its own does **not** reload the live filter, so you hear no change.
- **`MMSSB|Bandwidth=<hz>;`** — the *live/active* filter. Applies immediately (you see the passband widen), but on its own the QMX reverts it to the committed value.
- **`FW;` poll** — reads the *active* filter width; **the act of reading it makes the QMX re-assert a stale active width**, snapping any change back. (Proved by freezing the poll for 2 s: the filter holds, then snaps back the instant polling resumes.)

The working recipe (`poll_task` drain of `s_pending_ssb_bw`, set via `cat_request_ssb_bandwidth()`): write **`Filter RX`** (persist) **and** `Bandwidth` (apply live) to the same value, **and** while a width is pinned (`s_ssb_bw_pinned`) and mode is USB/LSB, **drop `FW;` from the poll rotation** so nothing reverts it. The BW label is driven optimistically from the user's selection (`ui_update_passband_width()` in `bw_preset_cb`). Trade-off: while pinned, a filter change made on the radio's own knob won't show on the Tab5 until you switch modes (FW; resumes in CW and on mode change).

Dead ends recorded so MM-token guessing isn't repeated: `MMSSB|Filter RX` *write* alone = persists but silent; `MMSSB|Bandwidth` *write* alone = audible but reverts; Kenwood `FW<nnnn>;` *set* = QMX returns `?;`; re-asserting the **same** mode digit (`MD2` while already USB) does **not** reload the filter (only a *different* mode does, which is why the old CW-bounce "worked" but flickered CW/50 Hz). CW (`MMCW|CW passband=`) commits cleanly on its own and is left untouched.

## Diagnostic logging (v0.15.11)

Opt-in field-diagnostics capture for remote bug reports. Toggle: **Diagnostic log** switch on the **top row** of the settings drawer (persisted to NVS as `diag_log`, applied at boot in `main.c`).

- `diag_log_init()` (called first thing in `app_main`, before any subsystem init) installs an `esp_log_set_vprintf` hook that captures **all** ESP_LOG output into a **512 KB PSRAM ring buffer** and always forwards to the serial console (passthrough — serial output is unchanged). The ring only fills while enabled.
- When enabled, `cat.c` also emits gated per-line **CAT RX** (`RX<- ...;`) at INFO so they're captured (INFO is always compiled in; don't switch to `ESP_LOGD` — DEBUG may be compiled out). **Poll de-duplication is essential**: the FA/MD/FW poll runs every ~50 ms (~60 lines/s) — logging each verbatim filled the old 128 KB ring in ~70 s. `diag_log_rx()` drops identical consecutive FA/MD/FW poll responses (logs them only on *change*; the `Freq=/Mode=/Passband=` change-logs already mark transitions), and the poll TX is not logged per-line — only a `poll heartbeat` line every 10 s. One-off writes (`Sent:`/`SSB filter ->`/`raw cmd:`) and all non-poll RX (MM, VN, ID, ?;, garbles) are logged in full. Net steady-state: ~0 lines/s + a heartbeat, so the 512 KB ring holds a whole session.
- Two extraction paths: **web** `GET /api/log` (downloads `qmx-log.txt`; "Diag log ↓" link in the web UI bottom bar) and **USB serial** (`tools/capture_serial_log.ps1`, works with no WiFi — the Travis case).
- The enable header logs Tab5 fw (`esp_app_get_description()`), `qmx_fw=` (from `VN;`), and build stamp, so every captured log is self-identifying.
- Ring concurrency: short spinlock for appends (lines come from a 256-byte stack buffer, so the critical section is tiny); the snapshot copies **without** the lock (head/count grabbed under it) to avoid a long interrupts-off window — worst case a few oldest bytes garble mid-copy, acceptable for a diagnostic dump. Never take a blocking lock in the vprintf hook (runs pre-scheduler and from arbitrary contexts).
- The switch is on the **top row** of the settings drawer (`DRAWER_SEC_DIAG`) and is also kept visible in **FT8 mode** (added to the `keep[]` list in `drawer_set_ft8_mode`).
- **`reset_reason` is ambiguous on this hardware.** The header logs `esp_reset_reason()`, but a deliberate reset-button force-off on the Tab5 comes back as `panic/exception`, not `external-pin`/`power-on`. So that value alone does **not** prove a software crash. The reliable tell for a genuine crash is a `Guru Meditation` / register+backtrace dump in the log immediately *before* the reboot — if there's no backtrace, treat `panic/exception` as "abrupt reset" (there is no app-level clean-shutdown path, so any forced power-off looks like this).

## RTC and time sync (v0.15.17)

`rtc/rtc.c` — thin driver for the Epson RX8130CE on I2C 0x32 (SYS bus, 400 kHz). Supercap 70,000 µF/3.3V gives ~30–40 h time retention. Registers 0x10–0x16 (SEC…YEAR, all BCD). Flag register 0x1D bit 0x80 = VBLF (power failure / low voltage).

`time_sync/time_sync.c` — global orchestrator. Init sequence:
1. `rtc_init()` — adds I2C device, runs M5Unified begin() sequence (bitOn 0x1F, clear 0x30/0x1E), reads and validates all BCD registers. VBLF=0 is **not** sufficient: an uninitialised chip has VBLF=0 but garbage registers. `rtc_get_time()` rejects any field out of range (sec>59, hour>23, year not 2000–2100) and marks invalid.
2. If RTC valid → `rtc_apply_to_system()` sets system clock immediately at boot via `mktime()` (safe: ESP-IDF newlib uses UTC timezone by default, so `mktime() == timegm()`).
3. Spawns `time_sync_task`: waits 15 s (head start for ft8_task), then waits for CAT ready (up to 5 min), queries `TM;` once on CAT connect, then loops every 5 min — catches QMX GPS lock events.

**QMX time-of-day only (no date):** `time_sync_notify_qmx(h, m, s)` reconstructs full UTC from the best available date anchor: system clock (if sane), NVS `last_unix_time`, or `EPOCH_SANE_MIN` (2023-11-14) as last resort. Both bounds are checked — stale anchors in the past AND garbage future values (>2040) are rejected. Date doesn't matter for FT8 (86400 % 15 == 0, so any day base gives the correct slot parity). NVS is only updated when the computed epoch is within sane bounds, preventing NVS poisoning from a garbage anchor.

**Sync priority (highest first):**
1. QMX GPS-disciplined (`time_sync_notify_qmx()` — GPS lock not yet detectable via CAT, so all QMX TM; is treated as potentially GPS)
2. Tab5 RTC — applied immediately at boot before QMX/SNTP are up
3. SNTP — writes to RTC + NVS always; only updates system clock when QMX has NOT synced in the last 5 min (avoids SNTP overriding a GPS-locked QMX)
4. QMX any clock — same code path as #1; periodic 5-min poll maintains time-of-day when offline
5. Manual — `time_sync_set_manual()`, always applies (rare POTA offline use)

GPS lock detection: the QMX CAT protocol has no GPS status command. Until one is identified, all QMX TM; responses are treated as potentially GPS-disciplined. SNTP defers to QMX (`QMX_DOMINATES_SNTP_MS = 5 min` in `time_sync.c`). Each accepted sync writes through to the RX8130CE so the clock persists across power-off.

**ft8_test.c** `set_time_from_qmx_rtc()` delegates to `time_sync_notify_qmx()` — no separate logic.

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

**Report value**: CQ-run sends a signal report built from the answering station's estimated SNR (`fmt_report`, clamped −24..+15, e.g. `-07`). SNR is computed in `ft8_estimate_snr_db()` (`ft8_test.c`) directly from the decoder's own FFT magnitudes — signal level (strongest of the 8 tone bins per symbol, averaged over the slot) minus the slot's mean noise floor, scaled from per-bin bandwidth to WSJT-X's 2500 Hz reference. Self-calibrating against the slot's own noise floor, but has no external ground truth; `FT8_SNR_CAL_OFFSET_DB` in `ft8_test.c` is a single tunable fudge factor if a real WSJT-X comparison ever becomes available.

**CQ-row filtering**: `ft8_qso_cq_filter_active()` is true throughout a CQ-originated session; `rebuild_list()` in `ft8_screen_view.c` then hides other stations' `CQ ` rows so replies to us stand out.

**Decode-list aging (v0.15.1)**: the list is a live picture of who's on frequency *now*, not a history log. `ft8_screen_get_all()` in `ft8_screen.c` expires entries not re-decoded within `FT8_ROW_STALE_SEC` (60 s) during the snapshot. The 1 Hz clock timer (`t_clock_cb`) sets `s_refresh_pending = true` every tick so stale rows drop even when the band is quiet and nothing new is decoded. "Heard: N" → "Active: N".

**No slot-skip**: `ft8_task` captures *every* non-TX slot, including the parity opposite an armed TX. With ping-pong decode a capture is exactly one slot (15 s) and ends on the next boundary, so the armed burst still fires on time — and capturing the opposite slot is the only way to hear the station we're working. (The old v0.13.1 parity-skip was removed in v0.15.0: it made us deaf on the partner's slots. The "~19 s swallow" it guarded against was pre-ping-pong, when capture+decode were synchronous.)

**UI**: TX confirmation modal has "Auto Pounce" button (visible for REPLY kind only) alongside "Transmit". The left-pane status label (`s_lbl_tx`) is always visible and shows — in priority order: ACTIVE (red), ARMED (amber), QSO complete (green), QSO timeout (orange, tap to clear), `ft8_status` passthrough (dim white).

**`ft8_status.c`**: tiny mutex-protected string written by ft8_task (RX capturing, decoding, TX symbol count), read by the 1 Hz LVGL timer. Shows what the FT8 process is doing at all times.

**`ft8_tx` extensions**: added `FT8_TX_KIND_ROGER_RPT` / `FT8_TX_KIND_73` and `extra_field[8]` to `ft8_tx_request_t`. `ft8_tx_build_request` now takes a `const char *extra` parameter (NULL = use `my_grid`, for backward compat).

## Branch state

| Branch | What | State |
|--------|------|-------|
| `main` | v0.15.18 — TX clash warning: `ft8_tx_is_clashing()` in `ft8_tx.c` scans the heard-station table against our armed TX tone (±1 bin / 50 Hz guard); ARMED/ACTIVE status label turns red-orange (0xFF4010) with "⚠ FREQ BUSY" when a collision is detected, normal amber (0xFFA040) otherwise. Deferred CAT mode write: `cat_request_mode()` in `cat.c` queues a mode change via `s_pending_mode_digit` (same poll-task drain pattern as `s_pending_ssb_bw`), avoids CDC race when mode and frequency writes come from the LVGL thread. WiFi on/off toggle: "WiFi initiated" checkbox in the settings drawer WiFi section, NVS-persisted (`wifi_en`), WiFi is only brought up when this is checked — useful for field/POTA use where WiFi is not needed. Show-password redesign: WiFi modal show/hide converted from an eye-icon button to a "Show password" checkbox below the password field; password textarea widened to 820px (was 700px + button). Drawer checkbox styling: IQ balance, flat-spectrum, diagnostic-log drawer controls converted from LVGL switches to themed square checkboxes via `make_drawer_checkbox()` helper in `ui.c`; consistent visual language across the drawer. FT8 left-pane spacing: `pad_all(16)` replaced with `pad_top(8)/pad_bottom(16)/pad_left(16)/pad_right(16)`; freq label y-pos 80→67, slot count y-pos 148→120, slot bar y-pos 159→131 — tighter layout. v0.15.17 — Global Tab5 RTC time sync: RX8130CE supercap driver (`rtc/rtc.c`), `time_sync` orchestrator, periodic QMX TM; poll, SNTP→RTC write-through; QSO override buttons (Re-send/RR73/73) during active FT8 exchange; "Show only CQ callers" display filter; SWR auto-reset after trip (TX;/RX; latch clear) — see CLAUDE.md RTC section. v0.15.15 — FT8 CQ-run reply filter modal + TX power/SWR readout: new "Filter" button on the FT8 screen opens `ui/ft8_filter_modal.c`, an include/exclude editor for the CQ-run auto-reply picker and the live decode list. Two "include" and two "exclude" fields (each independently toggled via plain checkboxes, not exclusive radios) are matched against the *whole* decoded message text (`ft8_call_t.last_text`) — not just the callsign — via `ft8_filter_match()`/`ft8_filter_contains_any()` in both `ft8_qso.c` (`scan_for_reply_to_me`) and `ui/ft8_screen_view.c` (`rebuild_list`). Each field accepts multiple space-/comma-separated terms (e.g. "POTA SOTA" or "JA, VK"), matching on ANY. Standalone "Exclude plain CQ callers" ORs into the existing `hide_cq` logic; "Exclude worked-before" is UI-only pending the ADIF log (v0.16.0). Persisted as a single `ft8_filters_t` NVS blob (`settings_set_ft8_filters()`). Modal layout iterated for big-finger touch: montserrat_24 throughout, 8px-padded checkbox indicators 16px clear of their textareas, Save/Cancel spread vertically down the right edge, placeholder text in the muted colour. Also added `cat_query_power_swr()` (`PC;`/`SW;` CAT query, valid only while keyed) and `ft8_tx_get_last_power_swr()`; the FT8 status line briefly shows "Last TX: X.XW SWRx.xx [Ns]" after each burst. v0.15.14 — Keyboard shift-label fix: the iPad-style abc/Abc/ABC shift cycle (`ui/ui_theme.h`) relabels the shift key to "Abc" in the pending single-shift state, but LVGL's built-in `lv_keyboard_def_event_cb` types any label it doesn't recognise as a control key ("abc"/"ABC"/"1#"/symbols are recognised, "Abc" is not) and runs *before* our cycle handler — so tapping Abc→ABC inserted literal "Abc" into the field. `ui_theme_keyboard_attach_caps_cycle()` now removes the built-in handler and makes `ui_theme_kb_shift_cb` the sole VALUE_CHANGED handler, calling the built-in explicitly for non-shift keys but never for the shift key. Not a delete-after-type fix: fields set `max_length`, so a full field silently drops LVGL's insert and a blind 3-char delete would eat real input. Reported by Michael KZ4LY. v0.15.13 — FT8 decode throughput fix: `ft8_estimate_snr_db()` recomputed the slot noise floor (a `powf` over the entire decoder waterfall) **per decoded message**, so a busy slot spent 9–18 s — longer than the 15 s slot — re-deriving the same value, overran the slot, and corrupted the *next* slot's concurrent audio capture; the decode list then filled with one slot-parity at a time and flipped/emptied. Split into `ft8_estimate_noise_db(mon)` (once per slot, lazy on first decode) + `ft8_estimate_snr_db(mon, cand, noise_db)` (cheap signal loop). Decode 9–18 s → ~1–2 s; both parities decode; throughput ~2×. Hardened the capture pipeline as defense-in-depth: 4-buffer pool + `s_buf_busy` in-use guard (capture never reuses an in-flight buffer; logs + drops instead of corrupting), `FT8_DECODE_BUDGET_MS=11000` safety net (now never hit), LDPC iters 60→30 (all in `ft8_test.c`). NOT core contention (tried decode→core 0, made it worse) and NOT buffer-reuse corruption — root cause was the per-message noise recompute. Also dynamic per-bin waterfall noise floor with adaptive black level. v0.15.12 — Sticky Panadapter/FT8 settings (switching modes saves/restores band/mode/bandwidth/frequency/zoom for each mode independently via `ui_save_snapshot()`/`ui_restore_snapshot()`, staged CAT writes on a short timer); FT8 "Preset: xx.xxx MHz" swipe-down opens the freq dropdown (screen-level hit area `s_ft8_freq_hit`, shown/hidden with the FT8 view - do NOT also foreground `s_container` on show(), as that's a near-full-screen opaque pane that covers the left/right edge-swipe grip handles when shown at boot in FT8 mode); UI colour theme consolidation (`ui/ui_theme.h` - shared `UI_COLOR_PRIMARY` tokens, single-cursor textarea focus, iPad-style abc/Abc/ABC keyboard shift cycle at montserrat_28) applied to CQ presets/identity/memory/FT8 TX/WiFi modals; WiFi password show/hide toggle; taller memory-grid cells; faint vertical "Stef OZ1LAV" operator-signature watermark drawn last (on top of waterfall/bottom-bar) and non-clickable. v0.15.11 — Diagnostic logging: opt-in **Diagnostic log** switch on the top row of the settings drawer (kept visible in FT8 mode) captures all ESP_LOG output to a 512 KB PSRAM ring (`util/diag_log.c`, vprintf hook installed first in `app_main`), downloadable via web `GET /api/log` ("Diag log ↓" in web UI bottom bar) or USB serial; persisted (`diag_log` NVS key). Rich enable header (Tab5+QMX fw, MAC, chip rev, reset reason, uptime, heap, callsign/grid, WiFi+QMX state). CAT RX logged per-line but **de-duplicated** (`diag_log_rx()` drops repeat FA/MD/FW poll responses; poll TX replaced by a 10s heartbeat) so steady-state is ~0 lines/s. QMX firmware read via `VN;` at link-up (`cat_get_qmx_fw()`, dev bench = `1_03_002`), shown in `/api/status` JSON, boot log, and diag header. `capture_serial_log.ps1` stamps a PC-side session header. README gained a top-of-file "Quick start" (cable/data-cable gotcha, one-finger edge-swipe nav + top-bar taps, required settings, what needs the QMX connected, diag log). Note: `reset_reason=panic/exception` also covers a button force-reset (no clean-shutdown path) — a real crash has a backtrace before it. v0.15.10 — Selectable SSB filter bandwidth in USB/LSB (2.5/2.7/2.9/3.2 kHz): writes BOTH QMX Menu-Manager items `MMSSB|Filter RX=` (committed, shows in radio menu) and `MMSSB|Bandwidth=` (live) to the same value, and drops `FW;` from the poll while a width is pinned (`s_ssb_bw_pinned`, USB/LSB only) since reading the filter makes the QMX revert it — SSB BW writes deferred to the poll task via `cat_request_ssb_bandwidth()`/`s_pending_ssb_bw` to avoid a CDC race; FT8 slot countdown bar glides smoothly (50ms `t_slotbar_cb`, ms range) and turns red during TX (`ft8_tx_get_status()==ACTIVE`); editable CQ presets (long-press Call CQ → `ft8_cq_modal`, 3 fields + radio select + "+ call grid" insert, NVS-persisted `cq_msg[3]`/`cq_sel`, Call CQ button label shows + short-tap transmits the active preset via `ft8_tx_build_request_text()` + general `ftx_message_encode`); freq keypad — top-bar opens empty, MHz/kHz keys replaced by a 10 Key/Phone digit-layout toggle (`s_freq_calc_layout`, digit read from button label at press) + Clear, mode row only shown for the Memory picker (`s_freq_picker_cb != NULL`); `settings_load_all()` now returns the live staged `s_pending` (instant reads, no NVS-flush lag); v0.15.9 — Edge-swipe gesture navigation replaces the burger button (right-edge swipe/grip opens settings drawer, left-edge swipe toggles Panadapter/FT8, bottom-edge swipe opens memory modal, all with "breathing" grip handles); snap-to-grid live tune cursor (cyan drag cursor jumps between absolute-frequency grid points instead of tracking raw touch, anchored independent of touch start); mode-aware snap steps changed (USB/LSB 500→250 Hz, FT8/digi/RTTY 100→500 Hz); fixed zoom>x1 overlay desync after retuning (`ui_update_frequency()` now calls `recompute_zoom_pan()`); top-bar/spectrum colour matching (passband-edge lines match BW label, VFO cursor matches Freq label, passband tint band at ~25% of that colour); v0.15.8 — Zoom-FFT passband centering at zoom>x1 (screen centers on the passband, not the dial, for off-center USB/LSB filters), amber VFO cursor and grey passband-edge lines repositioned to match and auto-recenter on CAT mode/filter-width changes (non-LVGL `recompute_zoom_pan()` to avoid an LVGL-cross-task freeze), per-zoom-level waterfall noise floor restricted to in-passband bins at every zoom level, zoom-FFT now engages on boot for persisted zoom>x1 (re-applied after `dsp_init()`), zoom-FFT EMA smoothing (alpha 0.6) across frames, removed bottom-bar memory-channel indicator, fixed memory recall not applying mode (CAT mode write was silently dropped by the 200ms TX rate-limiter, now sent via a delayed one-shot timer); v0.15.7 — S-meter no longer freezes once "RX: Capturing" starts (compute_and_publish_spectrum() now also runs every ~10 iterations inside the s_ft8_active branch on raw samples[]), FT8 freq preset label reworded to "Preset: xx.xxx MHz"; v0.15.6 S-meter redesigned as a visual tick-scale + bar (replaces "Signal: SX+Y" text), montserrat_22 labels, 0-68 scale (S1..S9+20); v0.15.5 memory-button frequency/mode picker, wider freq keypad with mode row, CAT mode-set-on-Enter rate-limiter fix, flat-spectrum floor reset on QMX power cycle (USB stays connected), S-meter fixed to track the IF-shifted VFO bin instead of the DC bin; v0.15.4 FFT-based FT8 SNR estimation; v0.15.3 top-bar Band/Mode/BW refresh fix + warmup round-trip, tap-to-enter frequency keypad, battery voltage + firmware version in bottom bar, RR73-as-grid fix, QMX RTC time sync for no-WiFi (POTA) FT8 timing, screenshot capture simplified | stable on ST7123 and ST7121 |

## Next up (v0.16.x)

ADIF logging: write each completed QSO to an ADIF file on-device; log view in FT8 screen; upload via web UI to LOTW/QRZ/eQSL/POTA.app.

"Worked before / new zone" highlighting in the FT8 decode list, once the ADIF log exists: color-code rows by whether that callsign/zone has already been confirmed — high value for POTA/SOTA activators chasing new contacts, low marginal cost once the log data exists.

Manual time-set UI: `time_sync_set_manual()` is implemented but has no UI entry point yet. Add a "Set Time" field in the settings drawer for rare POTA sessions where both QMX and WiFi are unavailable.
