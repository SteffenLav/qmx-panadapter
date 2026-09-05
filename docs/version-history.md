# Version History — "Shipped in" Roadmap (archived)

> Archived from `README.md` as of commit `cb417a7^` (e93fea4), the last revision before the
> Jun 18 2026 user-first README rewrite removed it. This is the full chronological "Shipped in"
> log from v0.1.0 through v0.16.0. Newest entries belong at the **bottom** (chronological order).
>
> Not maintained in the current README — kept here for history. Append v0.16.1+ entries below v0.16.0 if continuing.

## Roadmap

### Shipped in v0.1.0 *(untagged — development milestone)* — 2026-04-19 16:27 UTC

- **Phase 1 — LVGL UI on Tab5 ST7123 display.** Initial hardware bring-up: BSP init, PI4IO expander, MIPI-DSI panel, LVGL canvas running on the ESP32-P4. No radio connectivity yet — blank canvas, but the display pipeline was alive.

### Shipped in v0.2.0 *(untagged — development milestone)* — 2026-05-22 10:58 UTC

- **Phase 2.1** — USB Host running; QMX (STM32 VID=0x0483, PID=0xA34C) enumerates — 5 interfaces (CDC + UAC composite), 268-byte config descriptor.
- **Phase 2.2** — CDC-ACM open at 38400 baud; `ID;` → `ID020;` round-trip confirms Kenwood CAT emulation.
- **Phase 2.3** — Live frequency display via `FA;` polling. (Discovery: the Kenwood FA response is 14 bytes, not 15.)

### Shipped in v0.3.0 *(untagged — development milestone)* — 2026-05-22 13:32 UTC

- **Phase 3.1** — QMX USB descriptors confirmed: 5 interfaces; UAC IF3 alt1 EP 0x83 isoc mps=300 carries receive I/Q; IF4 alt1 EP 0x03 carries TX audio.
- **Phase 3.2** — UAC audio streaming from QMX: 48 kHz 2-ch 24-bit packed PCM at ~48000 pairs/s, coexisting with CDC-ACM. Key fix: `create_background_task = true` in `uac_host_install`; without this, UAC and CAT fight the USB host and audio drops. UAC component forked to `components/` to preserve the patch.
- **Phase 3.3** — Ring-buffered int16 stereo samples + stub DSP consumer task; producer/consumer balanced at 48000 pairs/s with no drops.

### Shipped in v0.4.0 *(untagged — development milestone)* — 2026-05-22 15:31 UTC

- **Phase 4.1** — esp-dsp integration: 1024-pt complex FFT with Blackman-Harris window, self-test passed (bin 100 detected, <1 ms).
- **Phase 4.2** — Real-time FFT replaces stub consumer: 1024-pt complex I/Q FFT running at 48 frames/s with no drops; spectrum data published via mutex for the render task.

### Shipped in v0.5.0 *(untagged — development milestone)* — 2026-05-25 18:19 UTC

The full display pipeline assembled end-to-end, plus touch-to-tune and landscape rotation:

- **Phase 5.1** — Real-time spectrum line graph at 30 Hz; fftshift maps bin order for the panadapter view.
- **Phase 5.2** — Waterfall with classic SDR thermal gradient; double-height 2× canvas scroll trick (no memmove, ~130 µs/tick instead of ~92 ms).
- **Phase 5.3** — Black label band with absolute frequency-offset tick marks and dB grid lines.
- **Phase 5.4** — EMA spectrum smoothing (α = 0.4) and autoscaling dB range (superseded by 5.5).
- **Phase 5.5** — Static −130/−30 dBm range following the manual Ref/Range convention of commercial SDRs; correct 24-bit → 16-bit scaling (continuous autoscale removed — it actively hid signal-vs-noise dynamics).
- **Phase 5.6** — One-pole IIR DC blocker on the I/Q stream before FFT.
- **Phase 5.7** — Polling `audio_task` on core 0 + drain loop; eliminates the slow ~13 s noise-floor-pumping cycle caused by the event-driven UAC path starving the FFT input with truncated chunks.
- **Phase 5.8** — dBm calibration: `DSP_DB_CALIBRATION_OFFSET = −148 dB` measured on a dummy load so the noise floor reads −130 dBm (S9 = −73 dBm).
- **Phase 5.9** — Larger fonts; continuous green spectrum curve with dim fill below, matching the design mockup.
- **Phase 5.10A–I** — CAT mode polling (FA/MD/FW round-robin at 50 ms); band derivation from VFO; real absolute-MHz frequency axis; S-meter; settings drawer with dB/EMA sliders and presets; 12 kHz IF offset compensation (VFO signal centered visually); waterfall auto-floor tracking; mode-aware tune snap (USB 500 Hz, CW 10 Hz, FT8 100 Hz); passband indicator lines from CAT FW; faster CAT poll + optimistic touch-to-tune; bigger touch targets (80×80 buttons).
- **Phase 6.1** — Touch-to-tune via CAT `FA;` with live cyan drag cursor.
- **Phase 6.2** — Landscape rotation 1280×720 via LVGL software rotation (`lv_display_set_rotation`, ~50% FPS cost, ~13 fps vs ~22 fps portrait — acceptable for a panadapter).
- **Cold-boot fix** — PI4IO I/O expander (holds LCD_RST / TP_RST low at chip power-on) must be initialised explicitly before display bring-up; soft resets mask the bug because the expander retains state across ESP32 resets.

### Shipped in v0.6.0 *(first tagged release)* — 2026-05-25 18:25 UTC

- **Panadapter feature-complete** through Phase 5.10I. First release distributed as a merged flashable binary. All display, DSP, CAT, and touch subsystems working end-to-end on real hardware with no developer tools required to flash.

### Shipped in v0.6.1 — 2026-05-26 12:54 UTC

- **Phase 5.10J** — Auto-enable QMX I/Q mode (`Q9 1;`) on CAT link-up, so the QMX starts streaming baseband I/Q immediately without requiring a manual menu step each session.

### Shipped in v0.7.0 — 2026-05-27 13:18 UTC

- **Hidden long-press screenshot** (Phase 5.11). Top-left 80x80 corner, 1 sec hold. Base64 streamed over UART; Python decoder saves PNG to `~/Downloads`.
- **I/Q balance correction** (Phases A–C). Gram-Schmidt blind adaptive image-rejection; toggle in settings drawer.
- **NVS settings persistence**. dB range, EMA alpha and IQ toggle survive reboots. Debounced flush minimises flash wear.
### Shipped in v0.8.0 — 2026-05-28 22:01 UTC

- **WiFi STA + on-screen credential UI.** ESP32-C6 co-processor over `esp_hosted` SDIO; SSID/password entered via a full-screen LVGL modal launched from the settings drawer; creds persist to NVS, no rebuild required to change networks. SNTP syncs UTC on connect — this is the prerequisite for onboard FT8 decoding.
### Shipped in v0.8.1 — 2026-05-29 07:45 UTC

- **Bottom status bar.** Battery indicator (level + charging) and WiFi state (SSID + RSSI in dBm) replace the dev-only FPS/PSRAM/IRAM line. Battery readout is stubbed pending INA226 wiring (see N6HAN's qrp_companion for the planned approach).
- **Drawer polish.** Drawer widened from 400 px to 520 px; IQ Balance row moved up under the title; presets (HF Normal / HF DX / Strong Sig) laid out side-by-side in a single row; on-screen keyboard buttons darker for better contrast.
- **Larger fonts.** Top-bar and bottom-bar text bumped from Montserrat 20 to Montserrat 24 to match the drawer.
### Shipped in v0.8.2 — 2026-05-29 12:23 UTC

- **Battery charging enabled.** The Tab5 BSP defines `bsp_set_charge_en()` but never calls it; v0.8.2 wires it (with QuickCharge negotiation) into `app_main` so the cell actually tops up when USB-C is connected.
- **Real INA226 battery readout.** Bottom bar now shows the actual battery percentage and charge state. Small dedicated I2C driver (`main/util/ina226.c`) reads the INA226 at address 0x41 on the main BSP bus. Voltage-to-SoC math and charging-detection thresholds informed by Zhenxing Han (N6HAN)'s qrp_companion battery indicator notes, in particular that on the Tab5 INA226 polarity is inverted vs the M5Unified docstring (negative shunt current = charging).
- **Last VFO persisted.** The last QMX frequency is saved to NVS on every CAT FA update (debounced, no flash churn) and shown immediately at boot. CAT then corrects within ~50 ms if the QMX has moved while the Tab5 was off.
- **Build noise cleanup.** Stale forward declaration removed; two BSP unused-variable warnings silenced with `(void)` casts.
### Shipped in v0.9.0 — 2026-05-30 13:46 UTC

- **Web UI.** Phase 1 + Phase 2 + Phase 3 of the remote panadapter front-end landed together: an HTTP status page on `/`, a polled `/api/status` JSON endpoint, and a binary `/ws` WebSocket streaming the live spectrum at ~10 fps. The browser canvas renders a continuous-curve spectrum (matching the device aesthetic) and a full waterfall with auto-tracking noise floor. See [Quick start: web UI](#quick-start-web-ui).
- **Unified visual identity.** Tab5 device and browser now share the same thermal waterfall palette (black→dark blue→teal→green→yellow→red), the same auto-tracking floor maths (median + 6 dB bias, 30 dB dynamic range above floor), and the same closed-polyline spectrum rendering. A signal of given strength looks the same in both places.
- **Pixel-perfect screenshots.** The Phase 5.11 long-press screenshot infrastructure now reliably round-trips through `tools/screenshot_decode.py` — the headline image of this README is its own output.
- **ESP-DSP fallback to portable C FFT.** The ESP32-P4 PIE/vector FFT (`dsps_fft2r_fc32_arp4.S`) crashed deterministically under sustained WebSocket load; falling back to `CONFIG_DSP_ANSI=y` removed the failure mode at the cost of ~10% FPS (35–42 vs 40–45). TODO: revisit once upstream esp-dsp PIE preemption handling matures.
### Shipped in v0.9.2 — 2026-05-30 16:56 UTC

- **Flat-spectrum mode.** The spectrum trace can now render against a per-bin tracked noise floor instead of absolute dBm. Noise variance collapses to a calm baseline near the bottom of the canvas; real signals pop sharp above it, matching the design mockup. Algorithm is identical browser-side and device-side: temporal EMA on incoming bins, asymmetric per-bin EMA floor (slow-up / faster-down so signals don't drag their own floor up), 5-bin spatial smoothing on the rendered trace, floor bias to lift the visible zero above the canvas bottom. Toggleable on both sides: browser via the `f` keypress, device via a `Flat Spectrum` switch in the settings drawer. NVS-persisted on the device so it survives reboots.
- **Screenshot mutex fix** (originally tagged as v0.9.1). The Phase 5.11 screenshot helper used a non-blocking display lock and could capture a partially-rendered frame, visible as a wrapped bottom status bar in the v0.9.0 hero image. Switching to `bsp_display_lock(portMAX_DELAY)` waits for the in-flight LVGL operation to finish before the snapshot starts.
- **Known issue.** The `-30 dBm` / `-130 dBm` axis labels at the left edge of the spectrum are still drawn in flat mode. They are misleading there because the axis is dB-above-floor, not absolute dBm. Will be hidden in a follow-up patch.
### Shipped in v0.9.3 — 2026-05-30 20:47 UTC

- **Hardware-revision boot diagnostics.** New `bsp_info_log()` prints a marker-fenced `=== TAB5 BSP INFO ===` block at boot listing chip revision, PSRAM size, panel/touch IDs (touch probed by I2C, panel inferred), heap, IDF version, and firmware version. Lets users with display or touch issues quickly identify which Tab5 hardware variant they have. Read-only and failure-tolerant - never touches the MIPI-DSI bus, never panics on a NACK. No behavioural change to the panadapter itself.

### Shipped in v0.9.4 — 2026-05-30 22:51 UTC

- **Persistent settings across firmware updates.** WiFi credentials, dB sliders, EMA alpha, IQ flag, last VFO, and flat-mode toggle now live in a dedicated `user_nvs` partition at offset 0x810000, well beyond the application image. Updates via web.esphome.io that use the standard (non-erase) write path preserve everything - flash the new firmware, the device comes back up with your settings intact. Root cause was `merge_bin --format raw` padding inter-segment gaps with 0xFF, including the default NVS partition at 0x9000-0xF000; the new partition is positioned where merge_bin output can never reach it.
- **DiGi label.** QMX mode 6 (used for FT8, JS8, RTTY via digi soundcard modes) now reads `Mode: DiGi` on the top bar instead of `Mode: FSK`. Touch-snap step (100 Hz) and passband geometry (USB-like) unchanged - cosmetic relabel only.


### Shipped in v0.9.5 — 2026-05-31 12:54 UTC

- **Browser top bar parity with Tab5.** The status row above the spectrum now shows VFO, mode, band, and S-meter (S-units and absolute dBm) updating once per second from `/api/status`. Amber centre marker on both spectrum and waterfall shows where the QMX dial is. Two grey passband edge lines on the spectrum mark the current filter window, mode-aware for USB / LSB / CW / AM / FM / DiGi. A frequency axis below the spectrum shows the centre MHz plus +/-12 and +/-24 kHz tick labels.
- **Responsive single-page layout.** CSS Grid with `100dvh` keeps everything on one screen with no scrolling, on phone portrait through 4K landscape. Spectrum-to-waterfall ratio 1:3. Narrow-screen breakpoint shrinks fonts and hides redundant pill labels. ResizeObserver matches canvas pixel count to the layout so rendering stays sharp at any size. Tap-target flat-mode button replaces the desktop-only `f` keypress (which still works).
- **`/ws` refactor.** The previous URI handler entered an infinite send loop and pinned the only httpd worker for the whole WS session, so `/api/status` requests queued without being served and the browser top bar froze the moment the spectrum stream connected. New architecture: URI handler captures `(server, fd)` on handshake and returns immediately; a dedicated `ws_push_task` runs the 10 fps send loop using `httpd_ws_send_frame_async` with static frame buffers. Status JSON and spectrum stream now coexist cleanly.

### Shipped in v0.9.6 — 2026-05-31 14:41 UTC

- **Hamlib rigctld TCP server on port 4532.** The Tab5 is now a Hamlib network rig adapter. WSJT-X, fldigi, N1MM and any other Hamlib-aware app on the LAN can talk to the QMX through the Tab5 -- frequency tracking, mode and passband control, S-meter reads. Configure your app for rig model "NET rigctl" (model 2) at `<tab5-ip>:4532`. Up to 4 concurrent clients; per-client tasks with 8 KB stacks. Supported commands: `f` `F` `m` `M` `v` `s` `t` `q` plus `\dump_state`, `\chk_vfo`, `\get_powerstat`, `\get_lock_mode`, `\get_vfo`. Unblocked by item 18 in the backlog -- WiFi STA + esp_netif lwIP sockets in place since v0.8.0.
- **CAT setters.** New `cat_set_mode(const char *)` translates Hamlib mode strings (USB / LSB / CW / AM / FM / PKTUSB / PKTLSB / RTTY / FT8 / CWR) to Kenwood mode digits and sends `MDn;`. New `cat_set_passband_hz(uint32_t)` sends `FWnnnn;`. Both share the existing 200 ms TX rate-limit with `cat_set_frequency`.

### Shipped in v0.9.7 — 2026-05-31 17:11 UTC

- DSP cleanup. The Phase 5.8 calibration block ran an insertion sort over 1024 floats every FFT frame to compute a median logged once per second. The calibration value is hard-coded and locked, so the runtime computation served no purpose. Removed; also recovers a 4 KB static buffer.
- Demoted dev-time per-second log lines (dsp Spectrum stats and audio RX stats) to ESP_LOGD. ESP_LOGW drop-warning paths in audio.c unchanged.
- Known issue: waterfall-scroll jumpiness reported during v0.9.6 testing is not fixed in this release. Root cause still under investigation. *(Resolved in v0.9.8.)*

### Shipped in v0.9.8 — 2026-05-31 19:52 UTC

- **Waterfall jump fixed.** Long-standing irregular ~1 Hz waterfall jump (visible since v0.9.5+) eliminated by dropping render task target rate from 30 Hz to 10 Hz. Root cause was an LVGL flush cascade: at 30 Hz target, `render_task` overran every iteration, leaving zero idle gap for LVGL to flush dirty regions; `display_unlock()` then priority-inherited the flush task synchronously, with accumulated dirty regions producing bimodal 47/73 ms flushes. At 10 Hz the ~68 ms idle gap per cycle lets LVGL flush small rects between iterations; unlock cost stabilises at ~26 ms, no visible jump. This is a workaround, not a deep fix - raising the rate again would bring the cascade back. Full diagnostic narrative in `release-notes/v0.9.8.md`.

### Shipped in v0.9.9 — 2026-05-31 22:10 UTC
Persistence + polish pass. Five quality-of-life features touching settings, touch-tuning, the waterfall, and the bottom status bar.
- **Last-VFO restore at boot.** Tab5 displays the last-known QMX frequency immediately on startup, eliminating the placeholder shown during the 1-2 second wait for the first CAT FA reply. Display-only - the QMX remains source of truth and overrides on first CAT poll.
- **Configurable CW sidetone pitch.** Touch-to-tune in CW / CW-R now offsets the dial by the configured sidetone frequency (default 700 Hz, range 400-1000 Hz in 50 Hz steps) so touched audio peaks land at the chosen pitch rather than zero-beat. Pitch is set in the settings drawer and persists across reboots.
- **Waterfall colour maps.** Four maps available: Thermal (original), Viridis, Turbo, and Grayscale. Switch instantly via a dropdown in the settings drawer. Selection persists across reboots.
- **Snap-to-strongest-bin on touch.** Touch-to-tune now searches +/-700 Hz around the touched position for the strongest spectrum bin and snaps to it (only when the peak exceeds local mean by >3 dB, so touches on empty noise floor still work as before).
- **Bottom-bar polish.** 3-zone layout: battery icon + percentage on the left, UTC clock (HH:MM:SS) in the center, WiFi symbol + SSID + RSSI + IP on the right. Bar height bumped from 30 to 36 px for comfortable icon rendering.
- **Drawer scrolling.** Settings drawer is now vertically scrollable to accommodate the new CW Pitch and Colour Map sections.

### Shipped in v0.9.9.1 — 2026-06-01 16:09 UTC
Trivial-debt cleanup pass. No new features.
- **Dynamic firmware version in boot log.** `bsp_info` no longer prints a stale hardcoded version string; uses `esp_app_get_description()->version` which the build system populates from `git describe`. Eliminates manual drift across releases.
- **Hide dBm axis labels in flat mode.** Carryover from v0.9.2. In flat mode the axis is dB-above-floor and the absolute `-30 dBm` / `-130 dBm` corner labels were misleading. Now hidden when flat mode is active.
- **Mojibake cleanup.** 14 instances of em-dash corruption (`â€"`, `Ã¢â‚¬â€`) removed from `main.c` and `wifi_config.c`. Replaced with ASCII `-` to be robust against future re-encoding round-trips.
- **README.** Removed stale "Network CAT bridge" entry from the longer-term roadmap (shipped in v0.9.6).

### Shipped in v0.10.0-beta1 — 2026-06-05 10:54 UTC

- **Onboard FT8 RX decoder.** Switch to a dedicated FT8 view from the settings drawer; the Tab5 decodes 15-second slots in real time on the ESP32-P4 using vendored [`ft8_lib`](https://github.com/kgoba/ft8_lib). Decode list with callsign, message, DXCC country, distance, bearing, SNR, and heard count. Operator identity (callsign + Maidenhead grid) configured via a new Identity modal in the drawer; persisted to NVS. See the [Onboard FT8](#onboard-ft8) section above for details.
- **FT8 view stability.** Pre-allocated row pool of 20 LVGL row containers with shared `lv_style_t` objects, refreshed in place via dirty-tracked `lv_label_set_text`. Eliminates the long-session reboot caused by `lv_obj_clean` + `lv_obj_create` cycling that fragmented internal heap and left stale draw queue entries.
- **Per-unit IF calibration trim.** New slider in the settings drawer (+/-200 Hz, 10 Hz steps, persisted to NVS) compensates for QMX local oscillator variance that shifts the 12 kHz IF baseband injection. Centralised the bin shift math in `ui_get_if_bin_shift()` so both spectrum and waterfall apply the same offset.
- **Beta status.** This is a public beta release. Stability across multi-hour FT8 sessions is not yet fully soaked. Please open an [issue](https://github.com/SteffenLav/qmx-panadapter/issues) if you see reboots or unexpected behaviour.

### Shipped in v0.10.0-beta2 — 2026-06-05 14:29 UTC

Hotfix on top of beta1. Opening the WiFi modal from the settings drawer in FT8 mode caused a reboot under specific timing. Root cause (only fully understood in beta3) was LVGL pool exhaustion under the FT8 row pool footprint. As a band-aid, `MAX_ROWS` reduced from 20 to 12. Reported by Ken (KF0AYY).

### Shipped in v0.10.0-beta3 — 2026-06-05 19:19 UTC

Real root-cause fix replacing the beta2 band-aid. LVGL's builtin allocator uses a static `.bss` array sized by `CONFIG_LV_MEM_SIZE_KILOBYTES` (default 64 KB); *all* widget allocations come from this pool, **not** the heap measured by `heap_caps_get_free_size`. At 64 KB the pool could not fit main UI + WiFi modal + identity modal + drawer + 20-row FT8 pool (about 110 KB cumulative).

- **LVGL pool moved to PSRAM and doubled to 128 KB.** Done by injecting `LV_ATTRIBUTE_LARGE_RAM_ARRAY=EXT_RAM_BSS_ATTR` through `idf_build_set_property()` (the conventional `add_compile_definitions()` does *not* propagate into managed components in this version of ESP-IDF). Internal heap actually *increased* by about 64 KB at boot as a side effect, since the static array's footprint moved out of internal SRAM entirely.
- **`MAX_ROWS` restored to 20.** Busy 20 m FT8 slots regularly produce 18-25 distinct decodes; the beta2 band-aid was truncating them. (Bumped again to 40 in v0.15.2.)
- **WiFi and identity modals + drawer pre-built at boot.** Eliminates first-tap stutter and removes the runtime allocation path that caused the beta1 crash.

Validated live on 20 m FT8 across 25+ consecutive slots with drawer + modal interactions mid-FT8 and no reboots; heap stable at 101-104 KB throughout.

### Shipped in v0.10.1 — 2026-06-06 19:46 UTC

- **Memory channels v2.** 32 NVS-persisted slots in a 4×8 scrollable grid. Tap to recall (CAT frequency + mode), long-press empty to save current VFO + label, long-press occupied to edit label or delete. Bottom-bar memory indicator shows active channel. Auto-clears on any VFO change. 200 ms modal-dismiss grace period prevents touch-bleed to waterfall.
- **FT8 decode colour coding.** RED = own callsign (priority), GREEN = "CQ " prefix, WHITE = other. Colours on callsign + message labels only.
- **`cat_get_mode_str()` helper.** Returns cached Kenwood mode digit as readable string (e.g. "USB", "CW", "DiGi").

### Shipped in v0.10.2 — 2026-06-06 20:39 UTC

- **IQ Balance setting now persistent.** Toggle state is restored from NVS on every boot/flash/power-cycle, no longer defaults to OFF.
- **Memory modal: keyboard dismiss on cancel.** Keyboard hidden automatically when cancel button pressed during label edit/delete.

### Shipped in v0.10.3 — 2026-06-06 23:16 UTC

- **CW mode frequency display.** Added 640 Hz LO offset correction for accurate dial alignment in CW mode.

### Shipped in v0.11.0 — 2026-06-07 15:51 UTC

- **Pinch-zoom and two-finger pan.** Pinch zooms the spectrum and waterfall from x1.0 (full 48 kHz view) up to x24.0. Two-finger drag pans the zoomed window. Double-tap resets zoom and pan to x1.0/centre. Zoom persisted to NVS.
- **Zoom indicator.** Top bar shows "Zoom: x1.0" in dim grey at full view, amber at any zoom level.
- **Frequency axis labels zoom-aware.** Tick labels update in real time as you zoom and pan. Resolution increases to Hz precision when the visible span is below 10 kHz.
- **Passband indicator lines zoom-aware.** Grey filter-width lines follow zoom and pan correctly.
- **Floating frequency tooltip.** Cyan label above the finger shows the target frequency in real time while dragging to tune.
- **Cyan cursor in waterfall.** Tune-cursor line extends across the waterfall as well as the spectrum.
- **CW LO offset read from QMX via CAT.** Read at connect time via `MMCW|CW offset;` and applied to bin shift math automatically.
- **CW trim slider.** ±100 Hz, 5 Hz steps, CW mode only, default −60 Hz. Persisted to NVS.
- **CW touch-to-tune corrected.** Tapping a signal tunes the dial to that signal's RF frequency in all modes.
- **Tap-to-tune precision at zoom.** Snap-to-peak disabled when zoom > x1.5; snap radius scales with zoom at lower zoom levels.

### Shipped in v0.11.1 — 2026-06-07 21:40 UTC

- **Top-bar quick-access control strip (Tab5).** Tap any label in the top bar to open a popup selector. Band popup reads all configured bands dynamically from the QMX at connect time. Mode popup switches USB/LSB/CW/DiGi. BW popup selects CW filter width (50–500 Hz) — CW mode only. Zoom popup selects ×1/×2/×4/×8/×16/×24 presets with pan reset to centre.

### Shipped in v0.11.2 — 2026-06-08 07:25 UTC

- **ST7121 display compatibility.** Tab5 units shipped after ~April 28, 2026 use an ST7121 display controller instead of ST7123, causing a blank screen with previous firmware. Auto-detects at boot via touch controller I2C firmware version (FW=1 → ST7121, FW=3 → ST7123). One merged binary works on both hardware versions.

### Shipped in v0.11.3 — 2026-06-08 13:00 UTC

- **Browser interactive controls.** Band, Mode, BW, and Zoom dropdown pills in the browser top bar mirror the Tab5 top-bar dropdowns. Commands sent via new `POST /api/cmd` endpoint.
- **Browser click/drag to tune.** Click or drag the spectrum or waterfall to tune the QMX. Cyan cursor with live frequency readout; mode-aware step rounding; commits on release.
- **Browser zoom + pan sync.** Spectrum and waterfall render the same zoomed window as the Tab5; frequency axis labels track the visible span.
- **Browser passband marker corrected.** CW passband symmetric around VFO centre; mode-default widths used when CAT has not yet reported BW.
- **Band memory (Tab5 + browser).** Switching bands returns to the last-used frequency on that band for the session.
- **`/api/status` extended.** Added `zoom`, `pan_bins`, `cw_pitch_hz`, `if_cal_hz`, `bands[]`.

### Shipped in v0.12.0 — 2026-06-08 22:04 UTC ⚠️ experimental TX — read the warning at the top

The long-planned manual FT8 TX path. **Read the [development warning](#️-development-firmware--ft8-transmit-is-experimental) before transmitting.**

- **Manual FT8 TX via CAT `TA;`.** Tap-and-hold a row in the decode list to reply to a heard station, or tap Call CQ to originate a call. A confirmation modal shows the exact message that will go on air, the audio frequency, and the target slot parity before anything is armed. The QMX does DDS synthesis and envelope shaping; the Tab5 only sends tone-frequency commands. No PC audio path required.
- **EVEN/ODD slot parity.** The slot countdown in the left pane now shows the current parity (EVEN in blue, ODD in amber). Reply parity is set automatically (opposite of the slot you heard the target in). CQ parity is user-selectable via TX: EVEN / TX: ODD buttons; default is any slot.
- **Auto-find clear audio slot for CQ.** Scans the current heard-station table for occupied 50 Hz bins and picks the nearest unoccupied slot to 1500 Hz.
- **Touch-and-hold row selection with scroll lock.** Finger-down for ≥ 400 ms enters selection mode; the row highlights and the list scroll locks so dragging the finger moves the highlight rather than scrolling the list. Lift confirms. A quick swipe still scrolls freely.
- **TX state indicator.** Left pane shows armed / transmitting status with slot parity, countdown, and tap-to-cancel/abort.

### Shipped in v0.13.0 — 2026-06-09 13:00 UTC

- **Auto search-and-pounce QSO state machine.** Touch-and-hold a CQ row and tap **Auto Pounce** (new button in the TX modal alongside **Transmit**). The engine arms TX1 and drives the full exchange automatically: TX1 (`<their_call> <my_call> <my_grid>`) → wait for their signal report → TX2 (`<their_call> <my_call> R<report>`) → wait for RR73/73 → TX3 (`<their_call> <my_call> 73`) → DONE. Each transition is triggered by scanning the decode list for the target callsign in the current slot. Timeout after 2 consecutive missed slots in any WAIT state.
- **Auto-Pounce button in TX modal.** The confirmation modal now offers both **Transmit** (fire once, manual) and **Auto Pounce** (hand over to the state machine) for REPLY-kind requests.
- **Persistent FT8 status bar.** Left pane shows a permanent status line below the slot countdown — what the FT8 process is doing at all times: capturing, decoding, TX armed/active, QSO state, or timeout. Written by the FT8 task; read by the LVGL 1 Hz timer via a mutex-protected string.

### Shipped in v0.13.1 — 2026-06-09 20:24 UTC

- **ST7121 touch controller support.** Tab5 units shipped after ~April 2026 carry an ST7121 touch chip (I2C 0x55, firmware version 1). Previous firmware entered a panic-reboot loop on these units because ST7121 NACKs two optional register reads that the driver treated as fatal. Fixed in the forked touch component: only `FW_VERSION_REG (0x0000)` is mandatory; the other reads are silently skipped. Also adds a `max_touches > 10` bounds clamp against a garbage register read. Both ST7121 and ST7123 now boot cleanly from a single merged binary.
- **I2C speed fix for ST7121.** ST7121 touch does not respond reliably at 400 kHz. Touch initialisation now always uses 100 kHz regardless of the system I2C clock setting.
- **Ping-pong dual-buffer decode.** A second PSRAM audio buffer and a dedicated decode task run in parallel: while slot N is being captured (15 s), slot N−1 is being decoded (~4 s). Every single slot is decoded — TX parity no longer causes a slot to be dropped. Previously, the slot immediately after a TX slot was silently skipped.
- **FT8 slot-skip fix.** `wait_for_slot_boundary` now tracks the previous slot start and returns as soon as any strictly-later slot boundary is seen, with no fixed arrival window. Eliminates the 30 s double-skip that occurred when a TX slot ended slightly past the boundary.

### Shipped in v0.14.0 — 2026-06-09 22:23 UTC

- **CQ loop mode.** Tap **Call CQ** — no confirmation modal. The engine picks the nearest unoccupied audio slot near 1500 Hz, arms immediately, and re-arms automatically after every TX slot, continuing to CQ every 30 s on the same slot parity until a station answers or you tap Cancel. The opposite-parity slot is always decoded so any reply triggers the automatic exchange.
- **CQ reply detection.** While in CQ loop mode, `ft8_qso_advance` scans every RX decode for any station sending `<my_call> <their_call> <report>`. Best-SNR caller is selected if multiple stations answer simultaneously. The CQ disarms and the reply TX is armed immediately without operator intervention.
- **SNR-sorted decode list.** CQ rows always appear at the top, sorted strongest-first; all other rows follow sorted by SNR descending. Replaces the previous by-UTC sort, making it much easier to pick the best DX to work.
- **E/O slot parity column.** First column in every decoded row shows **E** (blue, EVEN slot :00/:30) or **O** (amber, ODD slot :15/:45). Immediately visible which slot a decode came from, so you know which slot to transmit on when replying manually.
- **CQ timing fix.** `ft8_qso_on_tx_complete()` re-arms the CQ immediately after `ft8_tx_run()` returns (~T+12.7 s) rather than waiting for the decode task at T+19 s — the slot-boundary check at T+30 s now always finds the CQ armed and fires without missing a slot.

### Shipped in v0.15.0 — 2026-06-10 15:26 UTC

- **FT8 CQ-run mode.** Calling CQ is now a full auto QSO engine, not just a repeating call. The moment a station answers your CQ, the engine stops CQing, sends a signal report, runs the exchange (report → RR73 → done), then automatically resumes calling CQ for the next contact. Best-SNR caller is picked if multiple stations answer in the same slot. See [Calling CQ — CQ-run mode](#calling-cq--cq-run-mode).
- **Patient retry.** At every step — CQ cadence or mid-exchange — the current message is re-sent for up to 4 consecutive slots if the other station doesn't respond, instead of going quiet after one transmission. CQ-originated QSOs that time out mid-exchange resume CQ on the same frequency rather than dropping it; search-and-pounce QSOs go to a sticky timeout (orange status, tap to clear).
- **CQ-row filtering.** While a CQ-run session is active, other stations' `CQ` rows are hidden from the decode list so replies addressed to you stand out.
- **Auto Pounce documented.** The TX confirmation modal's **Auto Pounce** button (search-and-pounce QSO automation, shipped v0.13.0) now has usage docs — see [Replying to a station](#replying-to-a-station).
- **WiFi boot-loop fix.** Units with newer Tab5/ESP32-C6 WiFi co-processor firmware were rebooting endlessly a couple of seconds after boot — the GUI rendered fully every time, so this was never a display/touch issue despite earlier attempts to fix it as one. A serial log pinned the real cause: newer ESP-Hosted/C6 firmware auto-creates the default WiFi STA network interface and its event handlers once the C6 link comes up; the app then created a second one, so a single STA-start event ran the netif-start handler twice and the second `netif_add()` tripped an assertion, panicking the chip. Fixed by checking for an existing default STA interface before creating one, and by not starting WiFi at all on units with no saved credentials.

### Shipped in v0.15.1 — 2026-06-10 22:00 UTC

- **FT8 capture-window drift fix.** On FT8, decoding would gradually degrade and stop entirely after roughly 3 minutes — candidate count stayed high (~140/slot) but decoded count dropped to 0, even with strong signals on the air. A mode-bounce (FT8 → Panadapter → FT8) instantly restored decoding, for another ~3 minutes. Root cause: each capture waited for a fixed 180000-sample buffer (nominally 15.000 s @ 12 kHz), but the QMX's USB audio clock isn't bit-exact 48 kHz, so each capture took a hair over 15 s — the window slid later by ~0.2-0.4 s every slot until the FT8 signal fell outside the decoder's time-search window. Fixed by capping each capture at the next UTC slot boundary (zero-padding any shortfall), so the window stays anchored to the FT8 grid indefinitely. Field-tested: 20-30 decodes/slot continuously, no more die-off. Full root-cause writeup: [FT8 capture window drift](#ft8-capture-window-drift-resolved-in-v0151) under Quirks and trade-offs.
- **FT8 decode list live view.** The decode list now shows who's on frequency *now*, not a growing history: stations not re-decoded within 60 seconds drop off the list automatically, even while the band is quiet. "Heard: N" became "Active: N".

### Shipped in v0.15.2 — 2026-06-11 20:38 UTC

- **FT8 decode list bumped to 40 rows.** `MAX_ROWS` 20 → 40, with `CONFIG_LV_MEM_SIZE_KILOBYTES` doubled to 256 KB to fit the larger pre-allocated row pool. Validated stable on-device with the ping-pong dual-buffer decode (a busy band can now show twice as many simultaneous decodes without truncation).
- **Display brightness slider.** New "Display" section at the top of the settings drawer, above the waterfall colour map. Range 10-100%, persisted to NVS, applied on boot.
- **Persistent UI mode.** The panadapter now remembers whether you left it in Panadapter or FT8 mode and boots back into the same mode after a reflash or power cycle.

### Shipped in v0.15.3 — 2026-06-12 14:16 UTC

- **Top-bar Band/Mode/BW labels now refresh promptly.** The Band label could get stuck showing "Band: ---" forever if the very first `FA` response after link-up arrived during a UI-init race. `ui_refresh_band_label()` is now called unconditionally on every CAT `FA` poll (cheap, no side effects), and a one-shot `FA;`/`MD;`/`FW;` warmup round-trip right after `Q9 1;` link-up populates the top bar within ~1 second instead of waiting 7-10s for the CW-offset query and band-table scan to finish.
- **Tap-to-enter frequency keypad.** The top-bar "Freq:" label is now a button: tapping it opens a centered phone-style keypad. Type a number, then tap **MHz** or **kHz** to convert it to Hz (e.g. `1.5` + MHz -> `1.500.000 Hz`; `200.45` + kHz -> `200.450 Hz`), or type a plain Hz value directly. **Enter** sends it to the QMX via CAT and updates the display immediately; **Cancel** or tapping outside the popup closes it without changes.
- **Battery voltage shown alongside charge %.** The bottom-left battery indicator now reads e.g. "🔋 85% (8.1V)".
- **Firmware version in the bottom bar.** The running firmware's `git describe` version (e.g. `v0.15.3`) is now shown centered between the battery indicator and the UTC clock.
- **RR73 no longer mis-parsed as a Maidenhead grid square.** `RR73` syntactically matches the AA00-RR99 grid pattern (R is a valid Maidenhead field letter), so it was being recorded as the sending station's grid square, throwing off distance/bearing. It's now explicitly excluded.
- **QMX RTC time sync for no-WiFi (POTA) FT8 operation.** On SNTP sync, the Tab5 now pushes UTC time-of-day to the QMX's onboard RTC (`TM` CAT command) and persists the last-known UTC date to NVS. If SNTP is unavailable at boot (no WiFi), FT8 slot timing falls back to the QMX RTC time-of-day combined with the last-known date — accurate enough for 15-second slot alignment even if the date itself is stale. The FT8 task now waits indefinitely (with a periodic status update) for the QMX USB handshake instead of giving up after a fixed timeout, since persistent FT8 mode may restore before the radio is powered on.
- **Screenshot capture simplified.** Removed the hidden 80x80 top-left long-press UART screenshot dump; `screenshot_capture_rgb565()` (used by the web UI) remains the only capture path.

### Shipped in v0.15.5 — 2026-06-12 19:38 UTC

- **Memory buttons get a frequency/mode picker.** Long-pressing a memory slot now opens the frequency keypad (pre-filled with the slot's — or current — frequency and mode) before the naming keyboard, so both can be confirmed or edited together. The frequency keypad gained a row of DiGi/USB/LSB/CW mode buttons (dim-highlighted to show the active mode) and is 40% wider; opening it from the top bar pre-fills the QMX's current frequency and mode instead of a blank field. Memory buttons now show the channel name (large, centered) on the first line and mode + frequency (e.g. "USB  14.074.000 Hz", dimmed) on the second.
- **CAT mode-set-on-Enter fix.** Selecting a mode in the frequency keypad and pressing Enter previously failed to change the QMX mode — `cat_set_frequency()` and `cat_set_mode()` share a 200 ms CAT TX rate-limiter, so the mode command sent immediately after the frequency command was silently dropped. Fixed with a short delay between the two CAT writes.
- **Flat-spectrum floor reset on QMX power cycle.** With the QMX powered off and back on while the USB cable stays connected (no USB re-enumeration), the Flat Spectrum floor went stale and the display pegged at full-scale green until toggling Flat Spectrum off/on. `audio.c` now detects the silence/resume gap in the UAC audio stream directly and re-seeds the floor as soon as real samples resume — no cable unplug or mode toggle needed.
- **S-meter fixed to track the actual VFO signal.** The S-meter was pegged around S6 almost continuously, even on a quiet band between FT8 cycles, because its peak-detection window was centered on the raw FFT's DC bin (bin 0) — dominated by constant DC/LO leakage — rather than the VFO, which sits at the +12 kHz IF offset. `dsp_get_peak_dbm_around_vfo()` now takes the IF-shifted VFO bin as its center, so the S-meter (and the web UI's `signal_dbm` field) reflects real signal strength.

### Shipped in v0.15.6 — 2026-06-12 21:29 UTC

- **S-meter redesigned as a visual bar scale.** The top-bar "Signal: SX+Y" text label is replaced with a tick-labeled scale (S1, 3, 5, 7, 9, +10, +20) with small tick marks and a moving green bar beneath it, scaled 0–68 to match the existing S-unit mapping (6 dB/S-unit below S9, 1 dB/unit above, capped at +20). Tick labels use `montserrat_22` and are center-aligned over their tick marks. The freq label and zoom label were nudged to make room (`CENTER+30` / `RIGHT_MID-70`).

### Shipped in v0.15.7 — 2026-06-13 07:35 UTC

- **FT8 frequency preset picker.** The frequency shown under "MODE: FT8" is now a button reading "Preset: 14.074 MHz" — tap it to open a popup listing the conventional FT8 dial frequencies (160m through 6m) for every band the connected QMX actually supports, and tap one to retune instantly. The touch target covers the full label and extends downward for an easy hit. Fixed a conflict where this tap could land on the top-bar Band dropdown instead — both popups now check the current UI mode before opening.
- **FT8 slot countdown progress bar.** A small bar next to the EVEN/ODD slot countdown fills down over the 15-second slot, colour-matched to the parity (blue/amber), so you can see at a glance how much of the slot remains without reading the number.
- **FT8 status text enlarged.** The persistent status line under "Call CQ" (capturing/decoding/TX/QSO state) is now a size larger and easier to read at a glance.
- **Battery icon colour-coded.** The battery glyph in the bottom bar is pale green when full, pale yellow around half charge, and blinks pale red below 30% — the percentage/voltage text stays its normal grey.
- **Zoom indicator always coloured.** "Zoom: x1.0" in the top bar is now purple/magenta at all zoom levels (previously greyed out at x1.0, which made it look disabled).
- **Snappier frequency sync.** The top-bar Freq label now updates more promptly in step with the QMX VFO.
- **Fixed tofu/square glyphs in FT8 status text.** A handful of FT8 status strings ("Waiting for QMX...", QSO state messages) used an em-dash character not present in the bundled font, which rendered as a square. Replaced with a plain hyphen.
- **S-meter no longer freezes during FT8 capture.** The v0.15.5 fix kept the S-meter updating while FT8 mode was idle between captures, but per the "no slot-skip" design (v0.15.0) `s_ft8_active` is true almost continuously once FT8 is running, so that idle-branch refresh rarely got a chance to run — the S-meter would freeze the moment "RX: Capturing" appeared. The DC-blocker/window/FFT/dB/publish pipeline was factored into a shared `compute_and_publish_spectrum()` helper, now also invoked every ~10 iterations (~213 ms) from inside the active-capture branch, using the raw (un-mixed) I/Q samples so the spectrum stays aligned with the IF-shifted VFO bin the S-meter reads.

### Shipped in v0.15.8 — 2026-06-13 11:12 UTC

- **Zoom-FFT passband centering.** At zoom > x1, the screen now centers on the **passband (bw)**, not the dial/VFO frequency — important for USB/LSB where the passband sits well off to one side of the VFO. The amber VFO cursor and the grey passband-edge lines are repositioned to match, and re-center automatically when the QMX reports a new mode or filter width via CAT (previously this only happened on a zoom change).
- **Per-zoom-level waterfall floor, restricted to the passband.** The waterfall's auto-tracking noise-floor (median) is now recalculated at every zoom level, including x1, and sampled only from bins inside the passband — bins outside the passband run noticeably darker and were skewing the floor, hiding dim in-band signals (e.g. POTA stations in sunlight).
- **Zoom-FFT now engages on boot.** A persisted zoom level > x1 previously came back up as plain magnification (no extra resolution) after a power cycle, because `ui_init()` applies the saved zoom before the DSP zoom-FFT config exists. The zoom is now re-applied after `dsp_init()`, so saved zoom levels get full zoom-FFT resolution from first boot.
- **Smoother zoom-FFT spectrum/waterfall.** Added light EMA smoothing (alpha 0.6) across successive zoom-FFT frames — at high decimation each frame covers many more raw samples, so the display used to visibly jump between updates, especially noticeable at the lower effective fps of high zoom.
- **Fixed an LVGL freeze on QMX power-up.** The passband re-centering above is driven by CAT mode/filter-width updates, which arrive on the CAT task; an early version of this called LVGL APIs directly from that task without the display lock, freezing the Tab5 when the QMX reported its mode after powering up. Fixed by separating the non-LVGL recompute (pan offset + DSP zoom reconfiguration) from the LVGL label update.
- **Removed the bottom-bar memory-channel indicator.** Recalling a memory channel no longer shows a persistent "[M02] ..." label in the bottom bar.
- **Fixed memory recall not changing mode.** `cell_tap_cb()` sent `cat_set_frequency()` then `cat_set_mode()` back-to-back; both share the 200 ms CAT TX rate-limiter, so the mode command was silently dropped and only the frequency changed. The mode command is now sent via a short one-shot timer after the frequency write's rate-limit window.

### Shipped in v0.15.9 — 2026-06-13 15:34 UTC

- **Gesture-based navigation replaces the burger button.** The settings drawer, Panadapter/FT8 mode toggle, and memory-channel modal are now all opened by edge swipes instead of dedicated buttons, freeing up top-bar space and removing tune deadzones. A right-edge swipe (or a tap on the right-edge grip handle) opens the settings drawer; swiping right on the spectrum/waterfall or the open drawer closes it. A left-edge swipe right toggles between Panadapter and FT8. A bottom-edge swipe up opens the memory-channel modal, which now slides up/down instead of using a Close button (swipe down to dismiss). See the new [Gestures](#gestures) section for the full list.
- **Breathing edge-swipe handles.** The slim grip handles on the right, left, and bottom screen edges now slowly pulse opacity (in and out, ~1.4 s cycle) so they're discoverable without being visually intrusive.
- **Snap-to-grid live tune cursor.** While dragging on the spectrum/waterfall to tune, the cyan cursor and its frequency tooltip now jump between fixed grid points (e.g. ...200/300/400 Hz) instead of tracking the raw touch position — so you can see exactly which frequency will be tuned on release, before releasing. The grid is anchored to absolute frequency, not to the touch start position.
- **Mode-aware snap steps updated.** USB/LSB now snap to 250 Hz (was 500 Hz) and FT8/digi/RTTY now snap to 500 Hz (was 100 Hz), for both the live drag cursor and the on-release tune.
- **Fixed zoom>x1 overlay desync after retuning.** At zoom > x1, tuning to a new frequency (e.g. via touch-drag) left the passband-edge lines, VFO cursor, and frequency-axis labels positioned as if pan were reset to zero, while the spectrum/waterfall correctly re-centered on the new passband — the overlays would appear shifted to the right relative to the signal. `ui_update_frequency()` now re-derives the passband-centered pan after every frequency change.
- **Top-bar / spectrum colour matching.** The spectrum's passband-edge lines now match the BW label's colour, and the VFO/center cursor line now matches the Freq label's colour. The translucent passband-tint band (drawn behind the spectrum curve) uses the same colour at ~25% opacity.

### Shipped in v0.15.10 — 2026-06-13 22:01 UTC

- **Selectable SSB filter bandwidth (USB/LSB).** The BW dropdown now offers 2.5 / 2.7 / 2.9 / 3.2 kHz in USB and LSB (previously fixed at 2.7 kHz). The QMX exposes the SSB RX filter through its Menu Manager as two coupled items — a committed `Filter RX` (what shows in the radio menu and persists) and a live `Bandwidth` — and the Kenwood `FW;` poll re-asserts a stale width whenever it reads the filter back. The Tab5 now writes both items together and drops `FW;` from the poll while an SSB width is pinned, so a change applies immediately, sticks, and is reflected in the QMX menu. The old mode-bounce hack (which flashed CW/50 Hz on the top bar) is gone.
- **Smooth, TX-aware FT8 slot countdown bar.** The 15 s slot bar now glides continuously to zero (sub-second tick) instead of stepping once per second, and turns **red** while a TX burst is on the air (otherwise the EVEN/ODD slot colour).
- **Editable CQ message presets.** Long-press the FT8 **Call CQ** button to open a preset editor: three message fields with radio buttons (check the active one), an on-screen keyboard, and a `+ <call> <grid>` quick-insert that appends your identity. The Call CQ button label shows the selected message, and a short tap transmits it. Standard CQ constructions (`CQ DX OZ1LAV JO65FR`, `CQ POTA …`) and ≤13-char free text both encode via the general ft8_lib encoder. Presets persist to NVS.
- **Frequency keypad rework.** The top-bar `Freq:` keypad now opens with an empty field (type the new dial frequency from scratch); the Memory channel editor still pre-fills the stored value. The old MHz/kHz unit keys are replaced by a **10 Key / Phone** layout toggle (swap the digit grid between phone and calculator arrangements) and a **Clear** key. The DiGi/USB/LSB/CW mode row now appears only in the Memory editor — the top-bar keypad relies on the top-bar mode selector.
- **Instant settings reads.** `settings_load_all()` now returns the live staged state instead of re-reading NVS, so a value is reflected immediately after it's set rather than lagging the debounced flash flush (fixes the Call CQ label not updating right after Save).

### Shipped in v0.15.11 — 2026-06-14 07:01 UTC

- **On-device diagnostic logging.** A **Diagnostic log** switch on the top row of the settings drawer (kept visible in FT8 mode too) captures all firmware log output — plus per-line CAT request/response traffic — into a 512 KB in-RAM ring, with a self-identifying header (Tab5 + QMX firmware versions, MAC/serial, chip rev, reset reason, uptime, heap, callsign/grid, WiFi + QMX connection state). Download it over WiFi at `http://<tab5-ip>/api/log` (or the "Diag log ↓" link in the web UI), or capture it over USB serial with `tools/capture_serial_log.ps1` (no WiFi needed). The CAT poll logging is de-duplicated (identical FA/MD/FW responses dropped, a heartbeat every 10 s) so the ring holds a whole session instead of ~70 s. See the [Quick start](#quick-start--get-it-working-in-5-minutes).
- **QMX firmware version readout.** The Tab5 now queries the QMX firmware version via the `VN;` CAT command at link-up (e.g. `1_03_002QMX`) and surfaces it in the boot log, the diagnostic log header, and the web `/api/status` JSON (`qmx_fw`). (`ID;` only returns the emulated Kenwood model.)
- **README Quick start.** A new top-of-file Quick start: cable/data-cable gotchas, one-finger edge-swipe navigation and top-bar taps, required settings, what works without the QMX connected, and how to send a diagnostic log.

### Shipped in v0.15.12 — 2026-06-14 14:38 UTC

- **Sticky Panadapter/FT8 settings.** Switching to FT8 mode now remembers the Panadapter's band/mode/bandwidth/frequency/zoom and restores FT8's own last-used settings (or forces DiGi on first entry); switching back to Panadapter restores exactly what was left there. Frequency, mode, and filter-width writes are staged through `cat_set_frequency()`/`cat_set_mode()`/`cat_request_ssb_bandwidth()` on a short timer so the QMX has time to settle between writes.
- **FT8 "Preset: xx.xxx MHz" swipe-down.** Swiping down from the top edge anywhere over the FT8 frequency preset label now opens the frequency dropdown, matching the Panadapter top-bar gesture. (The touch target is a screen-level overlay shown/hidden with the FT8 view, sized to win against the Band/Mode top-bar hit zones that previously claimed top-edge touches.)
- **UI colour theme consolidation** (`ui/ui_theme.h`). Collapsed the "9 blues" of near-duplicate accent colours into a shared `UI_COLOR_PRIMARY`/`UI_COLOR_PRIMARY_BORDER` token, applied across the CQ preset editor, identity (callsign/grid) modal, memory channel modal, FT8 TX confirmation modal, and WiFi credentials modal. Shared helpers also standardise textarea/keyboard styling and add a blinking line-cursor on the focused field (only one field at a time).
- **iPad-style keyboard shift cycle.** On-screen keyboards (CQ presets, identity, memory labels, WiFi password) now cycle abc → Abc → ABC on the shift key, shown via the shift key's own label, and use a larger montserrat_28 font for better readability.
- **WiFi password show/hide.** The WiFi credentials modal gained an eye-icon button to toggle the password field between masked and plain text.
- **Memory channel grid readability.** Memory modal cells are taller (64px) with larger labels (montserrat_22/20) for easier reading and tapping.
- **Frequency keypad decimal handling.** Each `MHz.kHz.Hz` block is now capped at 3 digits and zero-filled on the right, so e.g. typing `1.5` gives 1.500 MHz (not 1.005 MHz) — digits land in the most-significant position of whichever block you're typing, matching how people actually read off a dial frequency. The popup overlay is darker (70% vs 50%), and the Cancel/Save buttons (renamed from "Enter") now use the shared danger/success colours with a visible border.
- **Operator signature watermark.** A faint, vertical "Stef OZ1LAV" reads bottom-to-top near the bottom-right corner of the screen on every screen, drawn last (on top of the waterfall/bottom bar so it's actually visible) and non-clickable so it never intercepts the edge-swipe gestures beneath it.
- **Frequency-axis polish.** A thin separator line now marks where the frequency-axis band meets the waterfall (matching the spectrum's dB grid-line colour), and the MHz tick labels are nudged up 3px for better alignment.

### Shipped in v0.15.13 — 2026-06-14 20:44 UTC

- **FT8 decode fix — both time slots now decode.** On a busy band the decode list used to fill with stations from only one 15-second slot at a time (all-even or all-odd), flip back and forth, and periodically empty — so you missed roughly half of every exchange. Root cause: the per-signal SNR estimate recomputed the slot-wide noise floor (a power average over the decoder's entire waterfall) **for every decoded message**, so a busy slot spent 9–18 s — longer than the 15-second slot itself — just re-deriving the same number. That overran the slot and corrupted the **next** slot's audio capture, which is why it alternated. The noise floor is now computed **once per slot** and shared across all messages, cutting per-slot decode time from ~9–18 s to ~1–2 s. Both slots now decode cleanly and the total number of decodes roughly doubled. The capture pipeline was also hardened so a slow decode can never corrupt a capture again: a small pool of capture buffers with an in-use guard, plus a per-slot decode time budget as a safety net.
- **Dynamic per-bin waterfall noise floor.** The waterfall now tracks the noise floor per frequency bin with an adaptive black level, so the background stays dark and even across the whole span (instead of a single global threshold that washed out quieter regions) while real signals still stand out.

### Shipped in v0.15.14 — 2026-06-14 21:19 UTC

- **Keyboard fix — the shift key no longer types "Abc".** On the on-screen keyboards (callsign/grid, WiFi password, CQ presets, memory labels), cycling the shift key **abc → Abc → ABC** inserted a literal `Abc` into the field on the middle tap. The pending-shift `Abc` label isn't one of the control-key labels LVGL's built-in keyboard handler recognises, so it typed it. The keyboard helper now fully owns key handling and never lets the shift key reach the text field. Thanks to Michael KZ4LY for the report.

### Shipped in v0.15.15 — 2026-06-15 22:35 UTC

- **CQ-run reply filter modal.** A new "Filter" button on the FT8 screen opens an include/exclude filter editor for the CQ-run auto-reply picker and the live decode list. Two "include" and two "exclude" fields, each independently toggled, are matched against the *whole* decoded message text — not just the callsign — so POTA/SOTA tags, grids, country prefixes, `/P`/`/M` suffixes etc. are all fair game. Each field accepts multiple space- and/or comma-separated terms (e.g. "POTA SOTA" or "JA, VK"), matching if the message contains ANY of them. Also adds standalone "Exclude plain CQ callers" (hide bare `CQ ...` rows, show only replies/exchanges) and "Exclude worked-before" (active as of v0.16.0 once the ADIF log is populated). Settings persist as a single NVS blob.
- **TX power/SWR readout.** After each FT8 TX burst, while still keyed, the radio is queried via `PC;`/`SW;` for instantaneous power output and SWR. The result is shown briefly in the FT8 status line ("Last TX: X.XW SWRx.xx [Ns]").

### Shipped in v0.15.16 — 2026-06-16 18:56 UTC

- **Browser web UI overhaul.** The built-in web panadapter now closely matches the Tab5 display:
  - **Top bar**: Band / Mode / BW / Zoom pills (left), VFO frequency in large amber centered, graphical S-meter (S1–S9+20 tick scale with live bar) between VFO and Flat/fps buttons (right)
  - **Bottom bar**: Battery % + voltage, Tab5 firmware version, UTC live clock (1 s tick from server epoch), WiFi SSID + RSSI, IP address, Diag log download link — equal-spaced flex layout
  - **Frequency axis**: 5 labels (L2 / L1 / centre / R1 / R2) with major tick marks aligned to actual label pixel centres + 3 sub-ticks between each pair
  - **Mouse-wheel tuning** with mode-aware snap: SSB 500 Hz, DiGi 100 Hz, AM/FM 1 kHz, CW 10 Hz
  - **European dot-separated Hz format** everywhere: `14.074.000 Hz`
  - **Optimistic VFO update** on click-to-tune and mouse-wheel (no poll lag)
  - **CW mode amber VFO line** drawn at dial + CW pitch offset (passband centre), matching the spectrum, waterfall and passband backdrop
  - **S-meter** labels S1 and +20 properly centred over end tick marks
- **`/api/status` extended.** New fields: `battery.mv` (millivolts), `flat_mode` (bool), `utc_epoch` (Unix timestamp), `tab5_fw` (firmware version string), `signal_dbm` (peak dBm around IF-shifted VFO bin).
- **WS frame byte[1] = zoom decimation factor.** Tells the browser whether the 1024 bins come from the base spectrum (decim=1) or from the zoom-FFT (decim>1), so residual zoom can be applied correctly.
- **Flasher renamed and fixed.** `tools/flasher/` is now `tools/QMX-Panadapter flasher/` to match the release zip name; `flash.command` had its executable bit restored after it was lost during the folder rename (caused a macOS "no appropriate access privileges" error on double-click).

### Shipped in v0.15.17 — 2026-06-17 14:51 UTC

- **Global Tab5 RTC time sync.** The Tab5's onboard RX8130CE supercap-backed RTC (I2C 0x32, ~30–40 h retention) is now initialised at every boot regardless of operating mode. Priority chain (highest first): (1) QMX/QMX+ clock — treated as potentially GPS-disciplined, always applied and records a 5-minute dominance window; (2) Tab5 RTC — applied immediately at boot before QMX/SNTP are available; (3) SNTP/WiFi — always writes to RTC + NVS, but skips `settimeofday()` when QMX has synced within the last 5 min so a GPS-locked QMX is not overridden; (4) QMX crystal (same code path as GPS, used when offline); (5) manual input for rare POTA sessions without WiFi or QMX clock (`time_sync_set_manual()`). A background task (`time_sync_task`) waits for CAT, queries `TM;` once at connect, then polls every 5 minutes to catch GPS lock events. Two bug fixes baked in: VBLF=0 alone is insufficient — uninitialised RTC chips have VBLF=0 but garbage BCD registers (e.g. year = 2085); all fields are now range-validated before the clock is trusted. NVS `last_unix_time` anchors are checked against both a lower bound (2023-11-14) and an upper bound (2040-01-01) to prevent a poisoned NVS value from propagating across reboots.
- **FT8 QSO override buttons.** During an active auto-QSO exchange (WAIT_RPT, WAIT_ROGER, WAIT_RR73), three buttons appear in the FT8 left pane: **Re-send** (amber — re-arms the current outgoing message immediately), **RR73** (blue — forces the RR73 message, skipping the report step), and **73** (green — fires the 73 sign-off and ends the QSO). Useful on a busy band where the auto-engine falls a slot behind or the state machine needs a manual nudge.
- **FT8 filter "Show only CQ callers".** New toggle in the CQ-run reply filter modal: when enabled, the decode list shows *only* rows where the station is calling CQ — useful when scanning for stations to work without exchange-traffic noise filling the list.
- **SWR auto-reset.** After each FT8 TX burst, if the post-burst `SW;` readback shows SWR > 4.0 (indicating the QMX SWR-protection latch tripped), the firmware automatically cycles `TX;` / 150 ms / `RX;` to clear the latch — so the radio is ready for the next TX slot without operator intervention. Requires QMX firmware 1.03.000+.

### Shipped in v0.15.18 — 2026-06-17 UTC

- **TX clash warning.** `ft8_tx_is_clashing()` scans the live heard-station table against the armed TX tone (±1 bin / ~50 Hz guard). When a collision is detected the ARMED/ACTIVE FT8 status label turns red-orange and shows "⚠ FREQ BUSY" instead of the normal amber — a visual heads-up before the burst fires so you can retune to a clear slot.
- **WiFi on/off toggle.** A "WiFi initiated" checkbox in the settings drawer WiFi section lets you disable WiFi entirely (NVS-persisted, default on). Useful for POTA/field sessions where WiFi is unavailable or unwanted — no WiFi startup, no SNTP, time comes from QMX or RTC only.
- **Deferred CAT mode write.** `cat_request_mode()` queues a mode change through the poll task via a `volatile` flag, the same pattern as the SSB bandwidth deferral — prevents a CDC pipe race when mode and frequency changes are triggered close together from the LVGL thread.
- **Show-password redesigned.** The WiFi modal's show/hide password control changes from an eye-icon button alongside the field to a "Show password" checkbox below it. The password textarea also widens to fill the full panel width.
- **Drawer checkbox styling.** The IQ balance, flat-spectrum, and diagnostic-log drawer controls are converted from LVGL switches to themed square checkboxes via a shared `make_drawer_checkbox()` helper — consistent visual language with the new WiFi toggle.
- **FT8 left-pane spacing.** Tighter padding (`pad_top(8)` instead of `pad_all(16)`) and upward nudges for the freq label and slot-countdown bar make better use of the narrow left pane.

### Shipped in v0.16.0 — 2026-06-18 UTC

- **ADIF QSO logging.** Every completed FT8 QSO is automatically written to an ADIF v3.1.4 file (`/spiffs/qso.adi`) on the Tab5's internal flash. The file includes CALL, FREQ (MHz, 3 d.p.), BAND, MODE, SUBMODE (FT8 — required by LoTW TQSL and eQSL for digital credit), RST_SENT, RST_RCVD, QSO_DATE, TIME_ON, MY_CALL, MY_GRIDSQUARE, and GRIDSQUARE. The total QSO count appears in the web UI bottom bar as a download link ("N QSOs ↓") that streams the file directly. A `/api/adif/clear` endpoint lets you wipe the log from the web UI. An in-memory worked-call cache (loaded from the file at boot) powers the "Exclude worked-before" filter toggle (already present in the filter modal since v0.15.15).
- **FT8 time sync fix — SNTP is now authoritative.** The previous sync priority treated every QMX `TM;` response as potentially GPS-disciplined, so a non-GPS QMX clock could override a fresh SNTP fix and corrupt FT8 slot timing. The new rule: SNTP always applies to the system clock; QMX time is only applied when SNTP has not synced within the last 10 minutes (e.g. field operation without WiFi). This means a WiFi-connected unit always uses internet time and the non-GPS QMX is only a fallback.
- **FT8 pounce TX frequency fix.** Tapping a CQ station to answer them previously used *their* TX tone as our TX tone — immediately flagged as "FREQ BUSY" (occupied by the station you just clicked). The panadapter now calls `ft8_find_clear_tone_hz()` at click time to pick a free slot, the same way CQ-run does.
- **FT8 CQ-run TX frequency fix.** When a station answered our CQ, the QSO engine overwrote our CQ tone with the caller's tone and sent subsequent messages (report, RR73) on their occupied frequency. Our CQ tone is now held fixed for the entire exchange; only the target callsign changes.
- **TX power reading corrected (×2 fix).** The `PC;` CAT response encodes power as `value = watts × 5` (not × 10 as originally assumed). The "Last TX" readout now shows the correct wattage.
- **Settings drawer font enlarged.** Section labels and control labels in the settings drawer use montserrat_28 (was montserrat_24) for easier reading at arm's length.

### Shipped in v0.16.1 — 2026-06-19 00:10 UTC

- **FT8 time calibration modal.** New `[HH]:[MM]:[SS]` panel accessible from the FT8 screen via **Filter → Sync Time** (`main/ui/ft8_time_modal.c`). The SS box auto-syncs from decoded FT8 signal timing (sub-second correction via `time_sync_apply_correction_ms()`); HH/MM are editable via numpad for manual override. A blue SS frame means live FT8 tracking — tap to lock. Apply uses `time_sync_set_manual()` when HH/MM are edited, `time_sync_apply_correction_ms()` for SS-only. The bottom bar shows "UTC(FT8)" after an FT8-derived correction, and `TIME_SOURCE_FT8` was added as a 5th time source.
- **WiFi crash fix on first credential save.** `ensure_sta_netif()` now defers the `WIFI_STA_DEF` guard to just before each `esp_wifi_start()` call. The old guard ran before the SDIO/C6 link was up, so on newer Tab5 units with newer C6 firmware a duplicate default-STA handler caused a `netif_add` assert the first time WiFi credentials were saved (Roy's case).
- **Context-aware Re-send button label.** The FT8 QSO Re-send button now shows what it will send — "Re-send / JO45", "Re-send / -07", or "Re-send / R-07" — depending on exchange state (`ft8_qso_get_cur_extra()` + `s_lbl_resend`).
- **FT8 row selection UX (POTA field report, Ken KF0AYY).** Hold threshold for row selection lowered 700 → 250 ms, an 80 ms dim preview highlight shows which row is targeted, and a 20 px jitter tolerance lets selection survive a shaky finger before the gate fires.

### Shipped in v0.16.2 — 2026-06-19 12:40 UTC

- **QRZ Logbook + eQSL ADIF upload from the web UI.** New "QRZ ↑" and "eQSL ↑" links in the web UI bottom bar upload logged QSOs directly. QRZ posts one record per request to `logbook.qrz.com/api` (API key prompted once, saved server-side); eQSL batches up to 20 records per request to `eqsl.cc` (username + password). Both track progress as a plain uploaded-count in NVS and stop on the first rejection rather than skipping past it. `/api/status` exposes `qrz_key_set` / `eqsl_creds_set`.
- **mbedtls PSRAM fix — outbound HTTPS unblocked.** Switching `CONFIG_MBEDTLS_MEM_ALLOC_MODE` to `MBEDTLS_EXTERNAL_MEM_ALLOC` moved the TLS handshake buffers (16 KB+) off the pressured internal DRAM into PSRAM. This was the firmware's first-ever outbound HTTPS connection (SNTP is UDP), so the path had never been exercised — without it the QRZ/eQSL uploads failed with `ESP_ERR_HTTP_CONNECT`.
- **FT8 sync precision fix.** The slot's sync offset is now a robust outlier-rejecting average across *all* decoded candidates in the slot, instead of a single-candidate value that could be a false sync hit (the "SS run-away" reports). Also fixed a sub-second rounding error in the time-sync modal's SS display and added a brief blue-border flash (`SS_FLASH_MS=30`) on every fresh measurement.
- **FT8 decode list readability (Ken KF0AYY).** Own-call rows now render inverted (red fill + white text) instead of red-on-black, which tested as more readable in the field.
- **QSO override buttons widened.** The Re-send / RR73 / 73 buttons now fill the full left-pane width (Re-send 20% wider than RR73/73, since it carries more text).
- **Freq keypad layout persists.** The 10-Key / Phone digit-layout choice now survives reboots (`freq_kp_calc` NVS key).
- **Web UI polish.** Bottom-bar IP address removed; action buttons reordered (ADIF / QRZ / eQSL / Diag / Tab5Shot) and restyled to match the top-bar pills' amber-hover look; new Tab5Shot screenshot link; S-meter scale labels enlarged and brightened; WiFi indicator swapped from emoji to SVG.

### Shipped in v0.17.0 — 2026-06-19 18:49 UTC

- **M5Stack Tab5 snap-on keyboard support (SKU A164).** New driver `main/keyboard/tab5_keyboard.c` — the keyboard is an STM32F030C8T6 I2C slave at 0x6D on its own bus (SDA=GPIO0, SCL=GPIO1, I2C port 1), driven in String mode so the MCU returns ready ASCII (no host keymap needed). 50 ms poll task; auto-detected at boot and silently disabled (with a bus scan logged) if absent. A C port of M5Stack's `M5Tab5-Keyboard-UserDemo` (reference + protocol notes in `docs/tab5-keyboard-ref/`). The earlier abandoned attempt failed because it assumed a TCA8418 at 0x34 on a bus the keyboard isn't on. UI bridge in `ui.c` (runs on the keyboard task under `display_lock()`): characters type into the focused textarea; Backspace/Del delete; arrow keys move the cursor; Tab cycles fields in the modal; Enter clicks the dialog's Save button; Esc clicks Cancel (registered per-modal via `ui_kbd_set_buttons()`, wired into all six modals; Enter/Esc work even with no field focused). The freq keypad (no textarea) routes digits/Enter/Esc straight into its buffer. Special keys arrive as spelled-out name tokens (`esc`/`tab`/`left`/.../`enter`) with inconsistent casing, matched case-insensitively.
- **WiFi SSID scan-and-pick.** A Scan button in the WiFi modal lists nearby APs (`wifi.c` async scan API: `panadapter_wifi_scan_start()` offloads radio bring-up + `esp_wifi_scan_start` to a task so the LVGL thread never blocks; `WIFI_EVENT_SCAN_DONE` handler dedupes + keeps strongest). Picking a network fills the SSID field with the exact-case beacon name, avoiding the case-sensitivity trap (a field report: a lowercase-typed SSID silently fails with reason=201 NO_AP_FOUND). The picker is a single bordered window — title + scrollable list + red Cancel — styled grey-on-dark to match the modal; the selected row turns blue with a check mark before closing.
- **Intermittent boot crash fixed (LVGL thread-safety).** `ui_init()` took the LVGL lock but released it after `ft8_screen_view_init()`, then kept creating widgets (top-bar hit-zones, edge gesture strips, signature, tooltip, pinch timer) **unlocked**, racing the `esp_lvgl_port` refresh timer (`lv_display_refr_timer` → `lv_obj_update_layout` → `get_prop_core`, NULL style deref). Latent on all units; hardware whose refresh timing hit the window crashed once or twice before booting (reported on newer ST7121 units, root-caused from a field serial log via addr2line). Fixed by holding the lock across the whole of `ui_init()`.
- **Password show/hide eye-icon button** restored (replacing the v0.15.18 checkbox), positioned under the Scan button; the password field was shortened to match the SSID field width.

### Shipped in v0.18.0 — 2026-06-21 UTC

- **Faster FT8 decode — replies land in time on busy bands.** The spectrogram is now built continuously *while* the slot is captured (instead of all at once after it ends), and decoding runs across both ESP32-P4 cores. The decode finishes early enough that a reply or report can go out on the *immediate* next slot rather than slipping a full 15 s cycle — the lag several operators reported on busy 20 m. The decoder still uses the same proven ft8_lib; this is about timing, not a different algorithm. A slightly lower candidate threshold also nets a few more weak-signal decodes, mainly on quiet bands.
- **dBm scale on the spectrum.** A labelled dB scale is back on the panadapter — absolute dBm in normal mode, dB-above-noise-floor in flat mode — and is now drawn on the browser spectrum too, which never had it.
- **Memory channels fixed.** Saving a channel could store a corrupted frequency (e.g. 14.020 MHz saved as 140 Hz) and refuse to recall; channels now store and recall the full frequency. *(Ian G4LXX)*
- **Tap-to-tune direction fixed.** Tapping the right of the panadapter to tune up could jump *down* in the far-right (aliased) quarter of the display; taps now always tune in the direction you tap. *(Ian G4LXX)*
- **Band switching fixed.** If you'd parked a band outside its legal allocation you couldn't switch back to it until a reboot; the band button now falls back to the band centre only when the remembered spot is out of band — otherwise it still returns you to where you were. *(Ian G4LXX)*
- **Web reconnect fixed.** The browser spectrum could get stuck on "reconnecting" after a reload or network blip while the rest of the page kept working; the WebSocket now hands the session to the newest connection, so it always reconnects.
- **WiFi reliability on new Tab5 units.** Hardened the C6/ESP-Hosted bring-up against a duplicate STA-start that could assert on some newer units. *(Roy)*
- **macOS flasher guidance.** Clearer help for the common Mac snags — `brew install esptool` (pip3 often fails on recent macOS), the Gatekeeper "unidentified developer" prompt, and running `bash flash.command` to avoid the chmod/permission hurdle. *(John K7JFW)*

### Shipped in v0.18.1 — 2026-06-22 UTC

- **Config backup / restore / edit (web UI).** New **Config ↓ / Config ↑** buttons in the browser bottom bar download and upload all settings *and* memory channels as one editable text file (`qmx-config.txt`, with `[settings]`/`[cq]`/`[ft8_filters]`/`[memories]` sections). Back it up before a clean flash and restore in seconds, edit everything in a text editor instead of on the touchscreen, or share just the `[memories]` section as a band plan with another operator. Import merges (only the keys present change). The file holds secrets (wifi_pass/qrz/eqsl) in clear text = full backup; strip those before sharing.
- **Clean / erase-all flash option.** The flasher now asks *normal or clean* just before writing. A clean flash wipes the whole chip first (`esptool write_flash -e`) — clearing a stuck stored state (e.g. WiFi that refuses to turn on). It erases **all** saved settings, so back up with Config ↓ first.
- **Memory channel recall fixed.** Recalling a channel could leave the Tab5 display frozen on the old frequency — and block band changes afterwards — while the QMX actually retuned. Recall now moves the display immediately and applies the saved mode reliably (through the CAT poll task, so it can't be dropped by the rate-limiter). *(Ian G4LXX)*
- **Panadapter ⇄ FT8 settings stick again — and FT8 is always DiGi.** Each mode reliably remembers its own frequency across switches, and entering FT8 now always forces the radio into DiGi instead of inheriting whatever mode (e.g. CW) the panadapter was on.
- **Reboot on fast mode-toggle fixed.** Quickly flipping Panadapter↔FT8 could spawn a second FT8 task that clobbered a shared decode queue and reset the unit; now guarded against a double-spawn. The CAT poll also rides out a transient USB hiccup instead of quietly stopping (which had frozen the on-screen frequency).

### Shipped in v0.18.2 — 2026-06-22 UTC

A stability patch — the headline fix is an idle reboot that hit anyone running with WiFi and the web UI open.

- **Idle reboot fixed (the big one).** Units left running with WiFi up and a browser connected to the web UI would reset after a few minutes of "idle" — caught on a serial capture as an internal-RAM exhaustion in the WiFi co-processor's SDIO receive path. The ESP-Hosted SDIO driver was running in *streaming* mode, which re-allocates a fresh internal DMA buffer for each larger burst; with the Tab5's tight internal RAM, sustained WiFi receive eventually couldn't get one and asserted. Switched the SDIO RX path to a recycled fixed-size buffer pool (`RX_NONE`), so after warm-up there are no runtime DMA allocations to fail. Soaked 13+ minutes with a browser connected vs. crashes at under 4½ minutes before. *(Caught via serial backtrace.)*
- **Web UI no longer freezes / needs manual reconnects.** Two coupled bugs: stale WebSocket sockets were abandoned without being closed, so a few freeze→reconnect cycles exhausted the LWIP socket table (`accept: ENFILE`) and the server stopped accepting connections; and a single transient send stall tore the whole session down. Now stale sockets are closed explicitly, transient stalls are ridden out instead of dropping you, TIME_WAIT is shortened so slots free quickly, the socket pool is larger, and the TCP window is bigger to smooth delivery. The waterfall/spectrum stream holds a steady 10 fps.
- **Bandwidth (BW) now changes from the web UI.** Picking a BW in the browser did nothing in CW mode — the web path sent a Kenwood `FW` filter command, which the QMX rejects (`?;`). It now sends the correct menu-manager command for CW (`MMCW|CW passband=`) and the three-write recipe for SSB, both routed through the CAT poll task (no command-race), and the BW value updates on both the Tab5 and the web pill.

### Shipped in v0.18.3 — 2026-06-22 UTC

Display ergonomics — waterfall tuning controls, an upside-down mounting flip, and a bigger Settings heading.

- **Waterfall display controls (new "Waterfall" drawer section).** Four live controls, all NVS-persisted, all visible immediately as new rows scroll in:
  - **Black level** (0–30 dB above each bin's noise floor that maps to black; default 9) — raise it to gate out near-noise haze and sharpen signal edges.
  - **Contrast** (10–80 dB span filling the colour ramp; default 45) — widen it so strong signals stop saturating into a fat blob and keep their shape.
  - **Adaptive floor** (0–100% blend between the per-bin adaptive floor and a single global floor; default 100) — dial back, or fully off, the per-bin tracker that can fatten signals.
  - **FFT window** (Blackman-Harris / Hann / Nuttall) — Hann gives the narrowest, sharpest signals; Blackman-Harris the cleanest skirts.
  The two mapping defaults moved from the old hardcoded +6 dB / 30 dB to +9 dB / 45 dB, which is already clearer on strong signals. This replaces the old compile-time-only flat-mode constants.
- **Flip display 180° (upside-down mounting).** A **Flip 180°** toggle at the very top of the settings drawer rotates the whole landscape view — for choosing which side the cables exit / aligning with the mount's edge features. Spectrum, waterfall, top bar and touch all follow automatically (pinch-zoom pan direction is corrected for the flipped case). Persists across reboots. The checkbox sits mid-row, not at the drawer edge, so it isn't toggled by accident when reaching for the drawer.
- **Larger "Settings" heading** for readability at arm's length.

*Also investigated but not shipped:* software I/Q image rejection (both a frequency-domain per-bin canceller and a manual by-eye null). Reverted — the ghost users were seeing is a **sample-rate alias** spaced exactly 48 kHz (the QMX's USB-audio rate) from the real signal, which is a QMX-side anti-aliasing characteristic and cannot be removed in the Tab5's DSP. True opposite-sideband I/Q images (within the window) remain handled by the existing adaptive I/Q balance.

### Shipped in v0.18.4 — 2026-06-23 UTC

Band-navigation upgrades, an FT8 decode fix, and a one-finger tuning gesture. (The auto-answer "robot" is built but **shelved** in this release — see the end.)

**FT8 decode**

- **Decoded tones are now recorded in real Hz.** Each decode's frequency was being stored as the decoder's internal coarse FFT-bin index (6.25 Hz per bin, measured relative to the start of the search window) rather than an actual audio frequency. Everything downstream treats that value as Hz — the reply tone the radio transmits on, the "find a clear frequency" CQ scan, and the busy-frequency clash warning — so all three were off by roughly 200 Hz plus a large scale error. Now converted to true audio Hz (using ft8_lib's own formula), so a station decoded at 1500 Hz is replied to at 1500 Hz. Verified on-air: 45 decodes across a busy 20 m, every one landing in the normal 200–2900 Hz FT8 passband, each station stable at the same frequency every cycle.
- **The station you're working decodes first.** While you're mid-exchange, the slot's candidate list is reordered so the partner's known tone is decoded before everyone else's. On a crowded band the decoder could otherwise spend the whole opening reply window working through unrelated signals, pushing your reply a full 15-second cycle late. This makes the existing reply-on-the-immediate-slot path fire on time when the band is full. (Pounce exchanges only.)
- **Band-aware "worked-before" filter.** The "Exclude worked-before" FT8 filter now keys off callsign **and band**, so a station you've worked on 20 m is still offered as new on 40 m.

**Band navigation & tuning**

- **Band-plan strip.** A thin colour strip below the frequency axis shows the CW / Digi / Phone segments of the current band with a marker tracking your VFO. A **Band-plan region** selector (Auto / Region 1 / 2 / 3) in the settings drawer shifts the segment boundaries to your IARU region.
- **One-finger pan ("stroll").** Drag the spectrum/waterfall horizontally with one finger to slide across the band; a live centre-frequency readout shows where you'll land and the radio retunes on release, clamped to the band edges. (Replaces the older two-finger drag; two fingers stays pinch-zoom.)
- **Snap-to-signal toggle.** A **Snap to signal** switch in the drawer (default on): tap near a signal and the VFO jumps to the strongest nearby peak; turn it off to tune exactly where you touched. Includes a fix so snapping works in **CW** — the search window now centres on the CW offset instead of the bare 12 kHz IF, where it previously missed the carrier (most visibly when zoomed in).

**Shelved / work-in-progress**

- **CW Audio** (demodulated CW to the Tab5 speaker) remains greyed out — it works but breaks up on the current USB-audio pipeline.
- **Auto-answer "robot"** (unattended CQ answering) is complete but **not yet on-air soaked**, and it keys the radio for real, so it ships **disabled**: the Filter-modal toggle is greyed and inert (tap → "Work in progress" toast) and the engine is hard-disabled in firmware, so it cannot transmit in this release.

### Shipped in v0.18.5 — 2026-06-25 UTC

**Band-aid fix for FT8 decode regression.** Root cause: e07f114 (CW audio shelved) introduced I2S/DMA contention and hot-path overhead that suppresses USB-audio pipeline throughput even when CW is disabled, degrading FT8 yield by 2–3× (avg 39→11 decodes/slot).

- **Regression root-caused via empirical bisect.** v0.18.0 sustained 39–45 decodes/slot; v0.18.1+ (which added e07f114 and d140485) collapsed to 11–15. Full revert of d140485 confirmed the decay was pre-existing (d140485 compounded it).
- **Two CW-audio hot-path calls disabled** (`cw_audio_preopen()` in main.c; `dsp_cw_forward()` in audio.c). Even when CW is off (default), these add I2S/DMA contention and per-sample-batch overhead on core 0, starving the concurrent UAC stream that FT8 decoding depends on.
- **Full d140485 commit reverted.** It introduced forced-mode changes on FT8 entry that, while helpful in isolation, couldn't overcome the underlying e07f114 pipeline bottleneck.
- **CW audio remains shelved** (greyed UI, no-op when disabled). Long-term fix requires full-rate UAC + async resampling or a dedicated core to avoid starving the audio consumer.

*(Diagnostic: late-session testing at fading-band time conflated band-conditions with regression, so the band-aid was tested on a quieter band; earlier same-day side-by-side comparison by the user proved the regression. The fix restores v0.18.0 decode yield on peak-band conditions.)*

### Shipped in v0.18.6 — 2026-06-26 00:24 UTC

**Correction to v0.18.5's claim above: it didn't restore v0.18.0 decode yield.** The night after v0.18.5 shipped, the user re-tested multiple releases back to back and found *no version after v0.18.0* sustained decode as well as v0.18.0 itself, even with the band-aid applied. Three more real regressions were found and fixed — but a same-night A/B afterward still showed a real, unresolved gap to v0.18.0.

- **`cw_audio_init()` was never disabled alongside `cw_audio_preopen()`.** The v0.18.5 band-aid disabled the two things CW audio actually *does*, but missed the task spawn itself: `cw_audio_init()` (`main.c`) unconditionally creates `cw_audio_task` at **priority 6 on core 1** — above `fft_task` (4, the audio ring's sole consumer for both the panadapter spectrum and FT8 capture) and both FT8 tasks (1) — looping forever on a 120 ms delay. Since the codec can never open with preopen disabled, the task does nothing useful, but it's still ~125 preemptions of `fft_task` per 15 s slot, all session, on every release since v0.18.1. Now also disabled.
- **3/4 of a separately-validated fix (commit `d140485`) was missing.** The v0.18.1 emergency revert threw it out entirely along with the actual e07f114 CW-audio regression it happened to be caught up with; only the FT8 double-spawn guard was ever cherry-picked back. Restored: the CAT poll task now tolerates ~20 consecutive transient CDC failures before giving up instead of dying permanently on the first one (was silently freezing the displayed frequency/mode for the rest of the session); FT8-entry "always DiGi" forcing and the Panadapter↔FT8 mode-restore step go through the reliable poll-task-routed path instead of a rate-limit-droppable direct write; the sticky-mode snapshot captures the freshest UI-commanded frequency instead of the lagging poll value; memory-channel recall uses an optimistic display update instead of a stale timer-based direct mode write.
- **`ui_push_spectrum()` was doing redundant LVGL work every tick.** This function runs at 10 Hz forever, on core 0, at the same priority as the USB audio producer — regardless of FT8/Panadapter mode, since the spectrum canvas is just hidden, not stopped. It was setting a label's opacity unconditionally every tick (LVGL can invalidate/redraw on a style-set even when the value is unchanged), a continuous cost competing with USB audio drainage. Now skips the call unless the opacity actually changed.

**Measured same-night, same-band, A/B testing (fading-band conditions, so the absolute numbers are conservative on all sides):** v0.18.0 averaged ~23 decodes/slot (5-slot sample); HEAD with only the `cw_audio_init()` fix averaged ~12/slot (8-slot sample); HEAD with all three fixes averaged ~15.2/slot (31-slot sample) — the third fix alone bought a real, statistically-meaningful ~25% improvement. **The gap to v0.18.0 is not fully closed.** Whether the remainder is a further residual regression or simply the band continuing to fade through the night (each test ran in a different few-minute window) is unresolved — next investigation should run a same-time-of-day comparison against v0.18.0 to settle it cleanly before chasing more code.

Also shipped this session:
- **FT8 CQ-run RST_SENT bug fix** — when a CQ-run responder sends RR73/73 immediately (skipping the normal signal-report exchange), `s_rst_sent` was never set, so the ADIF log recorded "599" instead of the responder's actual measured SNR. Fixed in `ft8_qso.c`'s `cqrun_answer()`.
- **FT8 distance-in-miles display option** — a "Distance in miles" checkbox in the settings drawer toggles the FT8 decode list's distance column between km and miles (reuses the existing great-circle distance calculation, just converts the displayed unit).
- **Bottom-bar diagnostic-log indicator** — a small red dot, anchored to the right edge of the battery-voltage text (tracks its actual rendered width via `lv_obj_align_to`, not a fixed offset), breathes (fades in/out) for as long as diagnostic logging is enabled — a glanceable reminder without opening the settings drawer. The firmware-version label (hidden in an earlier release to avoid overlapping the clock) is restored alongside it.

---

### Shipped in v0.18.7 — 2026-06-26 UTC

**FT8 decode-yield gap — CLOSED.** v0.18.6 left a real, unresolved gap to v0.18.0's decode yield after three regression fixes. Ran a controlled same-time-of-day A/B (midday, stable high-pressure conditions, 20 m) instead of another same-night test: built v0.18.0 in a separate worktree, full chip erase before each flash to eliminate any NVS/state carryover, two back-to-back 15-minute (60-slot) captures. Result: v0.18.0 and HEAD produced an *identical* 15.38 decodes/slot mean, with HEAD's distribution if anything slightly tighter (std dev 5.70 vs 6.65). The earlier "gap" was a band-fading confound — the prior test's two runs happened in different few-minute windows as the band faded through the evening — not a code regression. The v0.18.6 fix set (disabling `cw_audio_init()`'s ghost task, restoring `d140485`, skipping the redundant `ui_push_spectrum()` opacity set) is confirmed sufficient.

**FT8 auto-answer robot — un-shelved (live TX).** The robot (`main/ft8_robot.c`) was feature-complete since v0.18.4 but held back pending an on-air soak test that kept getting deferred. Decision made to ship it rather than wait indefinitely — the operator remains responsible for their own station, same as any other unattended digital-mode software. The "Auto-answer CQ with priority:" row in the FT8 Filter modal is un-greyed, `robot_en`/priority now actually persist, and a permanent (not one-time) "⚠ Transmits unattended — never leave running unsupervised" warning shows whenever the checkbox is checked.

**CQ tone auto-relocation.** Previously, `ft8_tx_is_clashing()` only showed a "⚠ FREQ BUSY" warning when another station drifted onto your CQ tone — the CQ kept transmitting on the now-occupied tone indefinitely, stepping on whoever was legitimately there, since clear-tone selection only ever happened once, at the moment Call CQ was pressed. `ft8_qso.c`'s new `relocate_cq_tone_if_clashing()`, checked on every CQ no-answer cycle, now re-scans and hops to the nearest still-clear slot when a clash is detected. An active exchange's tone still stays locked to the partner for the whole QSO, same as before — only idle CQ-between-cycles self-heals.

**Time sync: real priority bug fixed + new continuous FT8 auto-sync.** `time_sync_notify_qmx()` gated the QMX RTC fallback on "SNTP synced within the last 10 minutes" — but ESP-IDF's SNTP client only re-fires its sync callback roughly once an hour once synced, so that check looked "stale" for most of every hour even with WiFi healthy throughout, letting the QMX's free-running non-GPS RTC silently overwrite the system clock every 5-minute poll. Caught live, mid-QSO, as the FT8 slot clock jumping ~2 s off. Now gated on `wifi_is_connected() && wifi_time_is_valid()` instead, matching the documented "SNTP always wins when WiFi is up" priority.

Also new: the per-slot robust FT8 timing average (previously only applied via a manual Sync Time modal tap) is now **auto-applied to the system clock every slot** while FT8 is decoding — this tracks the actual on-air population's timing, which matters more for working people than absolute GPS/NTP truth if the two ever diverge. A damping gain (apply only ~30% of each slot's raw measurement) keeps it from chasing measurement noise — field data showed the undamped version oscillating ±200–300 ms slot to slot. The auto-sync path skips the QMX CAT push (a blocking, non-poll-task-routed CDC write) that the manual modal path still does, since that's unsafe to call from the decode hot path.

**FT8 own-call highlight fix.** The red own-call row highlight's callsign cache was only refreshed in `ft8_screen_view_show()` (i.e. on switching *into* FT8 mode) — setting your callsign via the CQ modal while already on the FT8 screen never re-triggered that, so the highlight stayed dead until a mode bounce. Now refreshed every cycle inside `rebuild_list()`, which already reloads settings every ~1 s.

**Filter modal layout fixes.** All eight checkboxes in the FT8 Filter modal now render at a consistent size — pixel-measured on real hardware that giving a checkbox actual label text made LVGL size its indicator ~30% bigger (41×41 vs 31×32) than a textless checkbox, and a style override on the indicator part had no effect. Fixed by making every checkbox textless with a separate label object placed beside it (the same construction the rows next to the textareas already used correctly), fixing an ordering bug along the way (label was aligning to the checkbox's pre-positioned default, not its final position). The four filter checkboxes are now stacked in one left-aligned column. Priority dropdown restyled to match the window's contrast (darker background, matching text colour on both the closed box and the opened list) and resized so "Most distant" isn't clipped at the bottom of the screen.

**FT8 distance-in-miles fixes.** The checkbox was in the panadapter-only "Snap to signal" drawer section, so it was invisible while in FT8 mode (the FT8 drawer hides non-FT8 sections) — and even when accessible, the decode-list column header was hardcoded to "KM" and never updated to "MI". Now in its own drawer section shown only in the FT8 drawer, with the header updating live with the setting.

**Recovery flasher port auto-detection.** `flash-recovery.bat`/`flash-recovery.command` (used after the v0.18.5-hotfix bootloader-corruption incident) hardcoded `COM3` / `/dev/cu.usbserial-*` respectively — a field report (Samuel W7STF) hit exactly this on a machine where the Tab5 enumerated as COM12. Both scripts now auto-detect the port the same way the main flashers do (low-number-first retry on Windows, broader device glob on Mac), and both also picked up the esptool v5+ `erase-flash`/`write-flash` hyphenated-subcommand fix the main flashers already had. The Mac script also had a stray `set -e` that would have aborted the whole retry loop on the first wrong-port attempt — removed.

---

### Shipped in v0.18.8 — 2026-06-27 UTC

**ARRL Field Day FT8 exchange mode.** Built and verified the same weekend as Field Day 2026 itself. FT8's CQ/QSO message format has a dedicated type for this (`WA9XYZ KA1ABC R 16A EMA` — the same one WSJT-X uses), but `ft8_lib` only had the message type *enumerated*, never implemented (confirmed also missing upstream in `kgoba/ft8_lib`). Implemented `ftx_message_encode_arrl_fd`/`decode_arrl_fd` in `components/ft8_lib/ft8/message.c` — the bit layout (`call1(28) call2(28) ir(1) class-number(4) class-letter(3) section(7)` + the standard `n3`/`i3` type bits) and the 86-entry ARRL/RAC section table were verified **byte-for-byte** against WSJT-X's own `packjt77.f90` source, not guessed at.

- **Settings:** `field_day_en` + `fd_class`/`fd_section` (e.g. `16A`/`EMA`), NVS-persisted. UI lives in the existing FT8 Filter modal: a checkbox plus Class/Section text fields.
- **QSO machine integration** (`ft8_qso.c`): the initial grid-exchange message is unchanged; the report-equivalent step carries class+section instead (no `R` the first time, `R`-prefixed when echoing back). Required generalizing the message-token parser (`split_msg3`) since the exchange field can now contain embedded spaces, which the old fixed 3-token `sscanf` silently truncated.
- **Call CQ auto-tags `CQ FD <call> <grid>`** while the mode is on, using FT8's existing "CQ modifier" mechanism (same as `CQ POTA`/`CQ DX`) — and **replaces** any other modifier the active preset carries, since the wire format only has room for one and FD signalling should win for as long as the mode is enabled. The long-press CQ preset editor reflects this: while Field Day mode is on, all three presets are dimmed and disabled (editing them is moot — the live behavior is forced) and a **live preview line**, computed via the exact same function the real TX path uses (never a second hand-maintained string), shows precisely what will be transmitted. Cancel is the only button left active.
- **ADIF logging:** `CONTEST_ID=ARRL-FD`, `STX_STRING`/`SRX_STRING` (literal `<class> <section>` text), `ARRL_SECT`/`MY_ARRL_SECT`.
- **Verification, given this is live-TX bit-packing code:** an independent Python re-implementation of the field math (10 round-trip cases); a boot-time on-device self-test (`ft8_arrl_fd_selftest()`) that encode/decodes 7 cases including the literal WSJT-X example messages and the `n3=3`/`n3=4` range-split boundary (`ntx`=1/16/17/32); and — the strongest available proof short of a real second station — an **end-to-end self-test** (`ft8_arrl_fd_e2e_selftest()`) that encodes a message, synthesizes its actual GFSK audio waveform (a heap-safe port of `ft8_lib`'s own `gen_ft8.c` demo synthesizer — the original uses ~620 KB of stack-allocated VLAs, fine for a PC demo but an instant stack-overflow panic on an ESP32 task), and feeds it through the **real** on-device `monitor_process`/`ftx_find_candidates`/`ftx_decode_candidate` pipeline — the literal code path live RF goes through. All confirmed PASS on actual hardware. (One real bug caught and fixed in the process: the e2e test crashed with a stack-protection fault when first run inline in `app_main` — the FFT/monitor machinery needs the same ~64 KB stack `ft8_task` gets, far more than the "main" task's 8 KB; moved to its own task.)
- A small fixed-keyboard-position UX bug was fixed along the way: the FT8 Filter modal's on-screen keyboard popped up at the bottom of the screen, covering the new Class/Section fields (which sit near the bottom of that modal) while typing — those two fields now pop the keyboard at the **top** instead; every other field in that modal is unaffected.

**FT8 simulation mode.** A `"FT8 Simulation Mode"` checkbox in the FT8 settings drawer (FT8 screen only, per request) lets you practice a full QSO — pounce, CQ-run, and Field Day exchanges — with zero real stations and, critically, **without ever keying a real, possibly-connected QMX**.

- **Phantom stations** (`ft8_sim.c`, new module): two fixed identities (`W1AW`/FN31, `K9ZZ`/EN52/class `5B`/`WCF`) periodically "call CQ" by running a real message through the exact same encode→GFSK-synthesis→decode pipeline the Field Day e2e self-test validated (`ft8_synth_and_decode()`, exported from `ft8_test.c` for this purpose) and injecting the result into the normal decode list via `ft8_screen_record_decode()` — so what shows up is something that genuinely round-tripped the real receive pipeline, not hand-built text.
- **Reply scheduling:** the module polls `ft8_tx_get_status()` for the rising edge to ACTIVE and reads `ft8_qso_get_state()` immediately after — which already reflects what `ft8_qso.c` just armed *this* TX to wait for — to decide the phantom's reply content (grid/report, RR73, or Field Day class+section) and fires it on the correct next opposite-parity slot. This module never reaches into `ft8_qso.c`'s internals; it only ever feeds the same decode-list input a real signal would, so the QSO state machine can't tell the difference — confirmed live: a full simulated Field Day QSO with K9ZZ logged to ADIF with `CONTEST_ID`/`STX_STRING`/`SRX_STRING`/`ARRL_SECT`/`MY_ARRL_SECT` all populated correctly.
- **Hard TX interlock** lives in `ft8_tx.c`, not in the simulator: `ft8_tx_run()` and `ft8_tx_arm()` both check `sim_mode_en` directly and skip every `cat_*` call (the `TX;`/`TA<freq>;`/`RX;`/`PC;`/`SW;` sequence, the Digi-mode pre-flight's `cat_set_mode()`, the poll-pause) — logged instead. The interlock is unconditional regardless of target callsign, so a real QMX connected during a simulated session never receives a byte.
- **Breathing red bezel:** a 10 px red border around the entire screen, pulsing continuously, shown the instant the checkbox is checked (any screen/modal) — added after the first version shipped with zero visual feedback, which looked like "nothing happens" when toggled. Re-foregrounds itself every second so a later-opened modal (also a child of the same LVGL screen) can't end up drawn on top of it.
- Simulated QSOs log to the same ADIF file as real ones (no special-casing — verified live), which is useful for testing the logging/upload path but means clearing the log afterward if you don't want practice contacts mixed with real ones.

**Flasher safety re-verified, not re-touched.** Given this session's Field Day urgency involved a lot of rapid iterate-build-flash-test cycles, explicitly re-checked `flash.bat`/`flash.command` against the v0.18.7 fix (write the full-chip `merge_bin` image at `0x0`, not `0x10000` — see [[project_merged_bin_offset_bug]]): both scripts are unchanged and still correct, with the explanatory comment intact. No flasher changes shipped this release; the merged binary/flasher zip for this version were deliberately **not** built or published as part of this wrap-up — held until manual on-device flash verification, per the operator's explicit request given the flasher's prior bricking history.

### Shipped in v0.19.0 — 2026-06-28 UTC

**FT4 TX: safety gate lifted.** FT4 RX was fully implemented and tested since v0.18.x (decoding at 0-3 stations per slot, 140-candidate pool, sub-millisecond latency on mode switch via buffer clear). TX was gated behind an unverified CAT cadence concern (48 ms per symbol vs FT8's 160 ms). Three consecutive FT4 CQ bursts fired on real QMX hardware with all ~105 TA commands sent cleanly at 48 ms intervals (~5 s on-air each), zero dropped commands, zero timeouts. Power/SWR readings normal (5.8W @ 1.28, 5.7W @ 1.28, 0W @ 1.00 — power varies within a burst). Safety gate removed from `ft8_tx.c` (`ft8_tx_arm()` and `ft8_tx_run()`); log message updated to reflect FT4 is now live. FT4 now operates identically to FT8 at the app level — only protocol-aware timing differs (7.5 s slots, 6.25 Hz tone spacing, 105 symbols @ 48 ms each).

**Buffer clear on mode/band switches.** When switching between FT8 and FT4 modes, or tuning to a different band within the same mode, the decode list now flushes stale decode entries. Prevents working old signals from a previous band/protocol context. New `ft8_screen_clear()` function (mutex-protected), called from `apply_freq_preset()` on mode switch and from `ui_update_frequency()` on band change (tracked via `s_last_band_idx`). Tested on hardware: builds clean, mode/band switches flush the list cleanly.

### Shipped in v0.19.1 — 2026-06-28 UTC

**New project homepage: [tab5.lav.dk](https://tab5.lav.dk).** There is now a dedicated website for the QMX Panadapter. It carries the full user guide, a quick-start, the hardware and troubleshooting reference, and a releases page — all as ordinary web pages you can read in a browser. If you'd rather not navigate a code-hosting site, this is the friendlier way in: open [tab5.lav.dk](https://tab5.lav.dk), read the guide, and follow the links to the download you need. GitHub remains the home of the source code and the actual release files (firmware and the one-click flasher); the website simply presents the same documentation in a more approachable form and points you to those downloads. Nothing about how you install or update the firmware changes — the site is purely a more comfortable front door.

**WiFi/upload robustness under full load.** Uploading a logbook (QRZ or eQSL) from the web UI while the panadapter was actively receiving FT8 could previously reboot the device or drop its WiFi. Two independent causes were found and fixed:

- **Reboot under load.** The WiFi stack's internal transmit buffers were drawn from a small pool of fast on-chip memory that the audio, FFT and display also depend on. Under a busy moment that pool could run dry and the device would reset. The fix moves the WiFi transmit buffers and the 64 KB audio capture ring into the large external PSRAM, leaving the on-chip memory free for the real-time signal path. Verified with 2.6 hours of continuous FT8 + web use, no resets.
- **Upload stalling.** Even without a reset, the secure (HTTPS) connection to QRZ/eQSL could time out while FT8 decoding kept the processor busy. During an upload or a log/diagnostic download the panadapter now briefly steps the FFT/FT8 work aside so the transfer can complete, then resumes automatically. In practice one FT8 cycle is skipped during the transfer and recovers on its own. This is a deliberate, light-touch trade so that routine file handling works without having to stop anything by hand.

Also adds a small always-on memory monitor to the diagnostic log for future field troubleshooting. No user-facing UI changes; settings, memories and logs are all preserved across the update.

### Shipped in v0.19.2 — 2026-06-29 UTC

**USB reconnect fix: QMX power-cycle/reconnect no longer breaks audio+CAT.** Root cause: once WiFi connects, internal SRAM fragments down to a ~31 KB largest free block; USB endpoint allocation for UAC (audio) and CDC-ACM (CAT) needs a contiguous DMA-capable internal block, so it could fail on a QMX reconnect or power-cycle that happened after WiFi was already up. First-boot connections always worked (heap still unfragmented at that point), which is why this symptom only showed up after a reconnect. Fixed by enabling `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y` — the ESP32-P4's USB DMA engine can address PSRAM directly, so USB transfer buffers now go there instead of competing for scarce internal RAM.

**microSD auto-archive.** When a FAT/exFAT microSD card is inserted, the diagnostic log, ADIF log, and a config export are now automatically mirrored to `/sdcard/qmx-panadapter/` in the background — no setup needed beyond having a card in. A red "SD" dot breathes in the bottom bar while a card is mounted. Card presence is detected by probing (the Tab5's SD slot has no card-detect line), so insertion/removal is picked up within a few seconds either way. (An on-demand "save screenshot to SD card" button was prototyped alongside this but removed before release — it reproducibly knocked WiFi offline by colliding with the WiFi co-processor link on a shared SD/WiFi hardware bus. The existing web-based screenshot is unaffected and unchanged.)

**Diagnostic log now always-on and persists across power loss.** Previously the diagnostic log was an opt-in switch and lived only in RAM (lost on power-off). It's now captured automatically from boot, and a rolling copy is also saved to internal flash, so a log from a field session is still available after a battery pull or power cycle — downloadable from the web UI ("Diag(saved) ↓") even with no SD card inserted. With a card inserted, the full session log is also mirrored to the SD card.

**Smaller heap fixes.** Two unrelated memory-pressure fixes bundled in alongside the above: the SD card's sector-size setting was corrected for real-world cards (was causing mount failures on some cards), and LVGL's internal memory pool was moved from a fixed 256 KB block of scarce internal RAM into the much larger external PSRAM, freeing that RAM up for the rest of the system.

**Crash fix: QMX disconnect/power-cycle could reboot the Tab5.** Found from a field serial capture (Dirk DK7CVD) reproducing a crash specifically after using the QMX's SWR Tune submenu, then power-cycling. Root cause was a race between two of our own background tasks: when the QMX drops off USB, the link task's cleanup closed the USB CDC handle while the poll task could still be mid-retry on that same handle (the v0.18.6 "tolerate 20 consecutive transient failures" tolerance widened this window enough to hit it in practice). The two collided inside the USB host driver and the resulting error was fatal. The link task now waits (bounded, a few seconds) for the poll task to actually exit before closing the handle.

**QMX IQ mode: now verified, not assumed.** The panadapter has always sent `Q9 1;` on connect to put the QMX into I/Q output mode, but only checked that the USB write itself succeeded - never that the radio actually accepted it. Added a readback (`Q9;`) right after, logged as confirmed-on or a clear warning with the raw response. This surfaced a real case: on the QMX `1_04` beta firmware, a user's radio showed IQ mode still disabled in its own System Config menu despite the write reporting success. **The `1_04` beta firmware is not yet verified with the panadapter at all** - CAT command behavior may differ from `1_03_002`/`1_04_002`'s tested baseline (e.g. `MD8;` is repurposed as a Tune-mode indicator on `1_04` and isn't currently recognized, just shown as an unlabeled mode). If you're on `1_04`, watch for this IQ-mode warning in the diagnostic log; staying on `1_03_002` remains the known-good combination for now.

**FT8 continuation messages no longer resend on every single exchange step.** A real QSO log showed every reply (report, RR73, 73) being transmitted twice. Root cause: the capture+decode pipeline for one slot (~15.1s capture + ~1.5s LDPC decode) takes slightly longer than the 15s slot itself, so decoding the partner's reply would routinely finish just *after* we'd already re-armed and re-fired the previous message for the next slot - guaranteed, not occasional, since most of a busy band's ~140 sync candidates are false positives that always burn the full LDPC iteration budget before failing (the decoder's only early-exit is on success). `FT8_LDPC_MAX_ITERS` lowered 30 → 15, cutting decode time roughly 25-30% on busy slots in on-air testing (~1.5s → ~1.0-1.3s). This reduces how often a continuation message needs a redundant resend; it does not fully eliminate it, since the capture window alone already runs slightly longer than the 15s slot before decode even starts - closing that the rest of the way would mean trimming the capture margin itself, which is deliberately conservative to avoid reopening the v0.15.1 clock-drift bug. Soak-tested live: QSOs complete correctly either way, this only affects how many cycles each step takes.

### Shipped in v0.19.3 — 2026-06-30 UTC

**FT4 clock-sync timing was silently wrong, now fixed.** The per-candidate timing-offset calculation (`decode_candidate_range()` in `ft8_test.c`) hardcoded FT8's block geometry (1920/960 samples) regardless of which protocol actually decoded — FT4 uses 576/288, so the computed offset was off by roughly 3.3× whenever it ran against an FT4 decode. A prior release had noticed the auto-sync *consumer* of this value would be unsafe for FT4 and gated its application off, but left the underlying calculation itself wrong, so the manual "Set and Sync the Clock" modal was still silently computing garbage offsets if ever used while in FT4 mode. Fixed at the source by reading the correct per-protocol fields already available on the decode candidate, and the FT8-only auto-sync gate was removed now that the math is right for both protocols. Three small UI follow-ups landed the same day once this was confirmed fixed: the bottom-bar clock now correctly shows `UTC(FT4)` instead of always `UTC(FT8)` while in FT4 mode; the Filter modal's "Sync Time" button (previously hidden in FT4, since it would have produced garbage corrections) is back; and the time-sync modal's hint text/flash label/log line say "FT4" when appropriate instead of always "FT8". Verified on hardware.

**Frequency-entry popup now remembers where you put it.** Requested by Samuel W7STF (groups.io #172940). Drag the popup by its "Enter freq" title label — the only non-button area — to reposition it anywhere on screen; the position is remembered across reboots and used the next time you open it, instead of always re-centering. Shared by both the top-bar keypad and the Memory-channel picker, since both use the same popup.

**Memory channel grid: drag-to-move, mode colours, and input validation.** Long-pressing a memory channel and dragging it onto an empty slot now moves that channel's saved frequency/mode there, snapping into place on release — previously the only way to do this was edit each slot by hand. Tapping an already-empty slot now opens the editor directly. Entering a frequency outside the legal band edges is now rejected immediately, before the editor even opens, instead of silently accepting it. Mode is shown before frequency on each button (was the other way round), and CW/DiGi/USB/LSB each now get a distinct, consistent colour shared between the memory grid and the frequency keypad's mode selector — USB was previously too close in shade to CW and easy to misread at a glance.

**Frequency keypad can now be resized, and a few rough edges removed.** Pinch or swipe up/down on the keypad to toggle between the normal and a smaller layout; your choice is remembered across reboots. The faint background dimming behind any popup (keypad, memory picker, and others) was lightened from ~70% to ~40% opacity so the spectrum and waterfall stay visible underneath instead of being mostly blacked out. Tapping outside the keypad no longer silently cancels what you were entering — Cancel or Enter are now the only way to close it.

**Band-plan strip improvements.** The coloured band-plan strip below the frequency axis now tracks your actual zoom and pan live (it used to show a fixed view regardless of zoom level), shows your current filter passband at band scale alongside the existing CW/Digi/Phone colour zones, and can be dragged directly to retune — tap to jump straight to a frequency, or drag to scrub with a live frequency readout before releasing. A couple of touch-handling bugs that let the band-plan strip and the spectrum underneath both respond to the same touch (causing odd double-actions) were also fixed.

**ADIF log viewer rebuilt as a proper aligned table.** The on-device QSO log viewer previously padded text with spaces to fake column alignment, which never actually lined up in a proportional font. It's now genuinely column-based, with each row's fields independently width-aligned. Also added: a sticky header row that stays pinned while you scroll, a Country column (looked up from the callsign), a Mode column (so FT8 and FT4 QSOs are distinguishable in the list, which they weren't before), the Report column split into separate Sent/Rcvd values, even-numbered rows lightly shaded for easier scanning on a long log, and a wider panel so nothing wraps awkwardly.

**FT8/FT4 transmit confirmation dialog improvements.** Reported by Dirk DK7CVD (groups.io item #17): added up/down nudge buttons to the TX confirm dialog so you can move to the row above or below without redoing the hold-and-drag selection gesture from scratch, plus a small colour legend clarifying what the green "Transmit" and blue "Auto Pounce" buttons each do. Selecting a decode-list row by a quick, deliberate tap now also works directly — previously a tap always required an arbitrary hold first before anything happened, which looked broken. The confirm dialog's countdown timer and title also now correctly account for FT4's different (7.5-second) slot timing instead of always assuming FT8's 15-second slots.

**FT4 decode quality fix.** A decoder iteration-count reduction intended specifically for FT8 (to reduce a known message-resend issue — see the v0.18.x notes above) had been unintentionally applied to FT4 decoding as well, since the two protocols share the same decode function. This noticeably hurt FT4 decode performance. FT4 now uses its own, separate iteration budget, restored to its original value — FT8 is unaffected.

**QRZ/eQSL upload reliability, round two.** The WiFi-stability fix for logbook uploads shipped in v0.19.1 turned out to be incomplete — a field report (with a live serial capture) showed WiFi could still die during an upload if a microSD card was inserted, because the v0.19.2 SD auto-archive feature didn't yet know to stay out of the way during an upload the way other subsystems already did. Fixed by having the upload process also briefly pause the SD archive task for its duration, plus a short staggered resume afterward (releasing all three paused subsystems at the exact same instant was itself creating a worse traffic spike than normal operation). Also fixed: the upload result popup always read "undefined QSOs uploaded" regardless of what actually happened, due to a mismatch between how the upload now reports progress and how the popup read it; and a duplicate internal field on every logged QSO that could cause QRZ to permanently reject all *future* uploads once one bad record was hit — both fixed, with older log entries automatically repaired in place so nobody needs to do anything.

**Web UI freeze duration capped.** A stale, half-dead browser connection to the live web view could previously leave it looking frozen for up to ~30 seconds before recovering, far longer than intended. Now capped at 5 seconds regardless of how slowly the underlying connection fails.

**Documentation:** the microSD Auto-Archive feature (shipped in v0.19.2) is now actually documented in the User Guide — the write-up was drafted at the time but never made it into that release.

**QMX IQ-mode handshake now retries, and tells you if it ever fails.** v0.19.2 added a readback check after the `Q9 1;` IQ-mode-enable write (see above) but only logged a warning on failure and moved on — the session then ran with IQ mode silently off for its entire duration. A field report (Dirk DK7CVD, groups.io #172933) traced a confusing, hard-to-reproduce symptom straight back to this: the panadapter signal would appear shifted, could be tuned across the *entire* 48 kHz window using the QMX's own VFO knob, and audio was only present once retuned back into range — only fixed by power-cycling the QMX. This is exactly what happens without IQ mode: the radio streams plain (non-IQ) audio instead of a centred baseband. It was initially misdiagnosed as a possible spectrum/waterfall rendering desync before Dirk's SD-persisted diagnostic log (a feature shipped in v0.19.2) showed the real cause: `QMX IQ mode NOT confirmed (raw='(no response)')`.

Fixed two ways:
- The `Q9 1;`/`Q9;` handshake now retries up to 4 times (300 ms apart) before giving up, instead of trying once. Verified live on the dev bench the same evening — the very first attempt reproduced Dirk's exact `(no response)` failure, and the second attempt confirmed IQ mode ON, both within under a second of each other.
- If all 4 attempts still fail (rare, but possible), a persistent red banner now appears across the top of the screen telling you immediately, with the suggested fix — instead of a log line nobody is watching live.

**Tab5 crash investigation opened, not yet resolved.** While investigating the above, Dirk's diagnostic log also showed 4 unrelated `panic/exception` resets in a single session — a real firmware crash, not a power-button reset. The diagnostic log can't capture the actual fault (panics bypass the logging hook entirely, by design — see CLAUDE.md's diagnostic-logging notes), so root-causing this needs a continuous serial capture (`tools/capture_serial_log.ps1`) from the field next time it happens. Tracked as TODO item #27b; no fix shipped this release pending that data.

### Shipped in v0.19.4 — 2026-07-03 UTC

This release is almost entirely about making **FT4 actually usable**. FT4 shipped in v0.19.0, but in practice it decoded poorly, kept resetting itself, and couldn't reliably complete a QSO. Four separate faults were stacked on top of each other — each one hiding the next — and all four are now fixed. On the bench, FT4 went from "no answers, lots of signal, confusing display" to completing and logging a real QSO.

**FT4 decode reliability — the big one.** The FT8/FT4 engine uses two capture buffers so it can decode one time-slot while receiving the next. On this hardware the internal memory is very tight, and one of those two buffers ended up with its FFT workspace in slow memory instead of fast memory — so *every other slot* ran its analysis about 10× too slowly and decoded nothing. FT8's longer 15-second slots mostly hid it; FT4's tight 7.5-second slots did not. Both buffers now share a single FFT workspace pinned to fast memory, which also frees up scarce internal memory as a bonus. Result: steady decoding on every slot instead of every other one.

**"FT8 stuck — resetting audio" no longer fires during normal operation.** A safety watchdog meant to recover a genuinely wedged receiver was mis-triggering on any quiet moment — including the middle of a QSO — and its recovery action wipes the decode list and briefly resets the receiver, which is exactly what was causing the "screen goes almost empty and slowly fills back in" you may have seen, and what was timing out otherwise-good contacts. It's now much harder to trigger and never fires while you're transmitting or mid-QSO.

**Slot clock no longer jumps around in FT4.** The Tab5 gently nudges its clock toward the collective timing of the stations on air (so you stay in sync with the people you're actually working). That nudge had no upper limit per slot, and FT4's noisier measurements were yanking the clock by up to ~¼ second every slot — enough to make the on-screen slot countdown visibly jump. The per-slot adjustment is now capped, so a genuine drift is still tracked smoothly but no single noisy reading can shift the clock visibly.

**FT4 EVEN/ODD indicator now correct.** The slot countdown and the EVEN/ODD markers were computed on FT8's 15-second grid even in FT4, so they flipped only every *other* FT4 slot (showing E E O O instead of E O E O) and disagreed with when the radio actually transmitted. All the slot-parity indicators now use the active mode's real slot length.

**QMX IQ-mode confirmation hardened again.** Following up the v0.19.3 handshake-retry fix, the IQ-mode readback could be fooled by the QMX's own command echo arriving at the wrong moment and report "confirmed" when IQ mode was actually still off (which streams non-IQ audio → 140 candidates, 0 decodes). The readback now waits for that echo to clear before checking, so the confirmation is trustworthy.

**Frequency keypad is now see-through.** The on-screen frequency entry pad (opened from the top bar) is now translucent, so the live panadapter stays visible behind it while you type. The Memory-channel version of the pad is left solid as before.

**Settings drawer tidy-up.** Removed two rarely-used items — **Snap to signal** (tap-to-tune now always tunes exactly where you touch) and the **FT8 sync lines** diagnostic — and moved **Band-plan region** directly under the Callsign & Grid button (its "Auto" setting is derived from your grid square, so they belong together). Also closed an empty gap that sat between Flat Spectrum and Presets.

### Shipped in v0.19.5 — 2026-07-04 UTC

Headline: **AM mode and Antenna Tune for QMX firmware 1_04_002** (field-verified on a real QMX+). Plus a round of interaction polish across the panadapter and two real bug fixes — including a crash on leaving FT8 mode.

**AM mode + Antenna Tune (QMX 1_04+ firmware only).** If your QMX/QMX+ runs the 1_04 beta firmware (reported over CAT), two new controls appear:
- **AM** is now selectable from the touch mode popup and the web UI mode dropdown.
- **Antenna Tune** — a dedicated window (from the settings drawer) that puts the radio into SWR Tune mode with a live SWR/power readout, and cleanly returns to your previous mode when you stop. It keys the radio continuously while active, so it gets a deliberate Start/Stop window (with a 60-second safety auto-timeout) rather than one-tap access.

Both are **invisible on the current 1_03_002 firmware** — they only appear when the connected radio reports 1_04 or newer, so there's zero change for everyone on the stable firmware. (AM has a single fixed receive filter on the QMX side; see the note at the end of this entry.)

**FT8 mode-exit crash fixed.** Leaving FT8 mode could occasionally reboot the Tab5 (a genuine crash, caught on a serial capture). The dual-core FT8 decoder has a helper running on the second CPU; on exit, the main decoder was tearing down the shared workspace after only a fixed short delay instead of waiting for the helper to actually finish, so the helper could briefly read memory that had just been freed. It now waits for the helper to fully stop first. If you saw a reboot when switching out of FT8, this was very likely it.

**WiFi on/off actually works now, and stops forgetting your password.** The WiFi on/off control (now an icon button inside the WiFi setup window) took effect only on the next reboot, so toggling it appeared to do nothing. It now applies immediately — off disconnects and stays off, on reconnects right away. Separately, the WiFi window used to blank the password field every time it opened, so pressing Save re-saved an *empty* password and the radio then couldn't rejoin a secured network. The field now pre-fills your stored password (masked), and Save respects the on/off toggle instead of always forcing a reconnect.

**FT8/FT4 remembers its frequency.** FT8 mode now keeps its own frequency across reboots and across switching back and forth to the panadapter, instead of sometimes opening on whatever unrelated frequency the panadapter was last on. Picking any FT8 or FT4 band preset saves it; a fresh boot into FT8 returns to that frequency (default 14.074 MHz).

**Band picker shows all bands at once.** On radios with many configured bands (QMX+), the band list is now laid out in two side-by-side columns so every band is visible without scrolling. A spurious "11m" entry that some radios report but don't actually have is now filtered out.

**Band-plan strip improvements.** The current-position indicator is now a framed, see-through window showing exactly the slice of the band on screen — drag it along the strip to tune, and the centre now always lands on a whole kHz. The 6 m band, which previously showed a blank strip, now has its band-plan segments. A tiny finger movement on lift no longer nudges the frequency.

**Point-to-tune is steadier.** Tapping/dragging on the spectrum or waterfall to tune now commits exactly where the cyan cursor last settled, ignoring the small finger-jump that happens as you lift off. The cyan guide line now also runs cleanly through the waterfall (following your finger, with no leftover trail) and stays glued to the spectrum line above it.

**Settings drawer sliders.** Sliders now respond only when you grab the knob, so a swipe up/down over a slider scrolls the drawer instead of accidentally moving the slider. Sliders pushed fully to one end now show the whole knob instead of clipping half of it. The **Settings** heading is always visible at the top when the drawer opens (it could previously open scrolled part-way down, looking empty). The **WiFi setup** button now closes the drawer when opened, matching the other full-screen windows.

**Memory recall in FT8/FT4.** Memory channels that aren't in the DiGi (data) mode are now greyed out and can't be recalled while you're in FT8/FT4 — recalling one would have knocked the radio out of data mode. They can still be edited and rearranged.

**FT4 no longer shows a false "stuck" message on a quiet band.** In FT4 mode a status pop-up reading "FT8 stuck — resetting audio" could appear when nothing was being decoded. Two problems, both fixed: it said "FT8" even in FT4, and the watchdog behind it (which resets the audio pipeline only after a long run with candidates but zero decodes) triggered after just ~60 s in FT4 versus ~120 s in FT8. Because FT4 is far less populated than FT8, an empty decode list is routine there, so it was crying wolf on a merely quiet band. It now waits the same ~2 minutes in both modes and names the correct mode. (Reported by Samuel W7STF.)

**Antenna Tune window.** The AM-mode and SWR-Tune support added in the previous update got its interface finalised: Antenna Tune is now its own window (opened from the settings drawer) with a bigger title and a live SWR/power readout, and the transient "transmitting"/"exited" pop-ups were removed since the on-screen state already makes it obvious.

**Documentation:** confirmed and recorded that AM mode has no selectable receive bandwidth — the QMX firmware uses one fixed wide filter for AM (and digital modes), so the "3.2 kHz" shown in AM is simply what the radio reports, not something adjustable. Only SSB and CW have selectable filter widths.

---

### Shipped in v0.20.0 — 2026-07-10 UTC

Headline: **a big robustness pass.** This release is mostly about making the device *stay up* — the WiFi drop-out, the screen freeze on opening a window, and the radio-link hiccups that field reports kept hitting are all now fixed or self-healing. On top of that: a decision to pause FT4, a decision to keep the web UI out of the way while FT8 is running, and a batch of interface, band, memory and battery improvements.

#### Reliability — the headline

**WiFi no longer dies until you reboot.** The long-standing "WiFi stops after a few minutes (FT8 and CAT keep working), and only a power-cycle brings it back" fault is fixed. It was a low-level lock-up in the link to the WiFi co-processor: once a certain oversized data burst arrived, the driver gave up on it and got stuck repeating the same failure forever. The link now recovers from that condition automatically (it drops one packet, which TCP simply re-sends) instead of wedging. Verified on hardware — the recovery fired during a live session and WiFi carried on without a blip. This affects everyone who uses the web UI or QRZ/eQSL upload.

**Opening a window no longer freezes the device.** Opening the ADIF log, CQ editor, Filter, or Sync-Time windows could occasionally hard-freeze the whole Tab5 — display, touch, and USB all dead, needing a power-cycle. It was traced (via a crash-log capture) to the faint on-screen operator watermark, which forced an expensive redraw whenever the screen fully repainted. The watermark is now drawn a cheaper way; it looks identical, and the freeze is gone.

**The radio-control (CAT) link rides out USB glitches.** A brief USB hiccup used to be able to kill the CAT link for the rest of the session (frequency/mode readout would freeze). It now retries and recovers instead of giving up, and only tears down on a genuine radio disconnect.

**The web UI stays out of FT8's way.** When you switch to FT8/FT4, the browser now pauses the live spectrum/waterfall stream and shows the log and upload controls instead — the streaming was competing with FT8 for the radio link and CPU. The streaming task was also de-prioritised so it can never interrupt audio capture. Net effect: steadier FT8 decoding and a more stable WiFi link while operating digital modes.

**SD card handling reworked for stability.** The microSD card now runs on its own dedicated bus instead of sharing one with WiFi, which removes a whole class of SD-vs-WiFi conflicts and frees up scarce memory. The automatic SD backup (mirroring your log/config to the card) is **currently disabled** — with WiFi now recoverable it's safe from *that* angle, but a live test showed the card being mounted squeezes memory enough to hurt FT8 decoding, so it stays off until that cost is reduced. Your log and settings are unaffected — they always live in the device's own storage; only the extra copy-to-card is paused.

**A dead FT8 capture task now restarts itself,** closing a gap where a fast Panadapter↔FT8 switch could leave FT8 running with nothing decoding behind it. And the web server is hardened against browser request pile-ups that could jam it.

#### FT8

**Decode timing fixed.** Each 15-second slot's recording is now anchored to the exact UTC boundary instead of starting slightly late, so the beginning of every signal (the part the decoder locks onto) is no longer clipped.

**Pile-up list.** Stations that answer you while you're mid-QSO used to vanish from the list once they stopped transmitting. They're now collected in a tappable "Pileup" list — tap a caller to work them, or tap ✕ to dismiss. The ADIF-log button turns into the Pileup button (different colour) whenever anyone's waiting.

**"Skip TX1" option** (Filter window) — start a pounce by sending your signal report straight away for a quicker QSO, with automatic fallback to the normal grid exchange if needed.

**Filter window polish** — wider spacing between the checkboxes (easier to tap just one) and the panel no longer runs off the bottom of the screen.

#### FT4 — paused

**FT4 is temporarily switched off in this release.** It was running the device out of memory and crashing. Disabling it keeps things stable and is fully reversible; FT8 is unaffected. FT4 will return once its memory use is brought under control.

#### Bands & tuning

- **11 m / CB band** support for QMX+ (band label, band picker, band-plan strip, and per-band frequency memory all handle it now).
- **Band-picker fix:** selecting a band now snaps to the nearest band centre, so 10 m and 11 m (which are close together) no longer get confused for each other.

#### On-device interface

- **Smooth backlight fade-in at boot** instead of a hard flash-on.
- **"Turn on / reboot your QMX" full-screen prompt** while the radio isn't connected yet, so a blank-looking screen at power-up is self-explanatory.
- **Memory channels overhaul:** the grid now ships with a few example channels so it isn't 32 blank cells on first use; a one-time ~10-second interactive tour shows that channels can be dragged and deleted; and there's a new **drag-to-wastebin** delete gesture.

#### Web UI (browser)

- **Whole-band plan strip** with colour-coded CW/Digi/Phone segments, a draggable "visible window" you can drag or tap to retune, and a VFO marker — mirroring the strip on the Tab5 itself.
- **Draggable divider** between the spectrum and waterfall (your split is remembered).
- **Screenshots now capture open pop-ups** (band/mode dropdowns), not just the base screen.
- **Frequency keypad**: tap the VFO to open it, drag it around by its title bar, standard 10-key digit layout, and no more screen-dimming behind it.
- **Fixes:** the centre frequency label no longer drifts off the true VFO; the flat-mode dB scale now matches the device exactly; the AM passband is drawn centred; the band dropdown reacts to a single click; and the FT8-mode notice text is now readable.

#### Battery

- **Battery-care charge limit** — optionally stop charging at a set percentage (default 80 %) to extend pack life.
- **Accurate charge percentage while charging** — the reading no longer jumps around, and the charge limit no longer sticks early or oscillates, now that the voltage rise under charging current is compensated for.

#### Under the hood

- The ADIF log viewer opens much faster (it was re-reading the whole file once per visible row).
- The developer resource-monitor overlay was removed from the settings drawer (it was only ever useful for debugging).

---

### Shipped in v0.20.1 — 2026-07-10 UTC

Hot-fix for a crash introduced in v0.20.0 — **everyone on v0.20.0 should update.**

**Fixed: the Tab5 rebooted every time you pounced a station.** The Skip-TX1 feature added in v0.20.0 put an ~11 KB scratch table on the stack inside the QSO-start routine, which runs on the display task's small (~8 KB) stack when you confirm a pounce — so it overflowed and triggered a "Stack protection fault" reboot on **every** pounce, whether or not you had Skip-TX1 switched on. The scratch table now lives in PSRAM instead. Pouncing (and Skip-TX1) work normally again — verified on-air. Reported by Dirk DK7CVD; reproduced and fixed the same day.

Nothing else changed from v0.20.0.

---

### Shipped in v0.21.0 — 2026-07-13 UTC

**FT4 is back**, the FT8/FT4 screen runs cooler, a nasty display glitch is gone, and QRZ uploads no longer get stuck.

#### FT4 returns

- **FT4 mode is re-enabled.** It was temporarily removed in v0.19.x because its faster 7.5-second cadence starved the processor on this hardware and could crash. The root cause is now fixed (see below), and FT4 has been verified end-to-end: it decodes every slot, and full FT4 *and* FT8 contacts were completed, logged, and uploaded during testing.

#### Runs cooler in FT8/FT4

- **The panadapter no longer draws itself while you're on the FT8/FT4 screen.** The spectrum and waterfall were still being rendered and rotated ~30 times a second even though the FT8 screen completely covers them — pure wasted work on the busiest processor core. Stopping it freed a large amount of headroom (the core went from nearly saturated to mostly idle in FT8/FT4 mode), which is exactly what let FT4 come back. The S-meter in the FT8 top bar keeps updating as before.

#### Display glitch fixed

- **Fixed: a full-screen flash ("blink to blank and back") that appeared every 15 seconds to a couple of minutes, mostly in FT4.** Two internal 10-second housekeeping tasks briefly blocked interrupts long enough to make the display controller drop a single frame. They've been rewritten to do their work without that stall. Verified: 10+ minutes of FT4 with zero flashes.

#### Logging

- **QRZ upload no longer gets stuck on already-uploaded contacts.** If a contact was already in your QRZ logbook, the upload used to stop dead at that record and never reach the newer contacts behind it. It now skips duplicates and continues, so newly logged contacts always get through. (Genuine errors like a bad API key still stop the batch, as before.)

#### Maintenance & recovery

- **New: reset settings from the web page**, without needing a computer or flashing tool. Two scoped choices — reset just the app settings, or just the Wi-Fi/network state — each with a confirmation step. Useful for clearing a stuck configuration in the field.
- **The QMX's VOX is now switched off automatically** when the panadapter connects, the same way IQ mode is set up at link time — one less thing to configure on the radio.

## v1.0.0 — 2026-07-16 — The 1.0: a complete standalone FT8 station

**The beta label is gone.** Every v1.0 release gate is met: the QMX Panadapter is now a
complete, self-contained FT8/FT4 station — receive, transmit, auto-QSO, logging, and
upload to **all three major logbooks (QRZ, eQSL, and now LoTW)** — with no PC in the loop.
33 commits since v0.21.0.

#### LoTW upload — the final v1.0 gate (live-verified)

- **On-device TQ8 signing + upload to ARRL Logbook of the World**. Each QSO is
  RSA-SHA1-signed with your own LoTW callsign certificate and uploaded directly to
  lotw.arrl.org — no TQSL program, no PC. The TQ8 format was reverse-verified against
  tqsllib's actual source code (the ARRL help pages are wrong in places), with a
  host-side verification harness (`test/lotw_harness.c`).
- **Guided 2-page web setup**: step-by-step TQSL certificate export instructions (with a
  button straight to ARRL's own how-to page), then a browser-side .p12 import — the
  certificate passphrase never reaches the device (parsed in-browser). Certificate
  renewal: Ctrl-click the LoTW button to re-run setup.
- **Live-verified 2026-07-14**: a real TQSL certificate imported, 22 QSOs signed
  on-device and accepted by lotw.arrl.org.
- Importing a (new) certificate rewinds the upload cursor so the whole log is re-signed
  under the new key (server-side duplicates are harmless).

#### FT8/FT4 exchange quality — the double-send is dead (field-verified on air)

- **Hold-for-decode TX gate**: when a reply is due at the slot boundary but the previous
  slot's decode is still in flight, the burst holds ~1.7 s for the fresh reply instead of
  re-firing the stale message. The "every message sent twice" behaviour that doubled QSO
  duration is gone. Field-verified across ~20 live cycles and multiple QSOs.
- **Nonstandard-callsign support**: special/compound calls (`YR50NADIA`, `PJ4/...`) no
  longer decode as `<...>` once heard in full, and their answers to your CQ are now
  recognized. Verified live.
- **RST_RCVD finally logged correctly in CQ runs**: the partner's numeric roger
  (`R-06`) is their measurement of *your* signal — it is now captured as RST_RCVD
  instead of logging the 599 placeholder.
- **Broken QSOs can be resumed**: if a partner fades mid-exchange and the machine gives
  up, a 5-minute resume record lets the exchange continue where it stopped — either
  automatically when they are heard calling you again, or by re-pouncing their row. No
  more starting over from grid TX1.
- **Decode list behaviour during CQ runs**: stations on your own TX slot parity (which
  you physically cannot hear while transmitting) no longer age out mid-run — and are
  hidden from the list while the run is active, returning the moment it ends. The
  "list suddenly empties a minute into my CQ" effect is gone. The stale window widened
  60 → 120 s for less churn on marginal signals.
- **Pileup replies send a signal report** (report-first, correct for the CQ-side role)
  instead of a grid TX1 (Ken KF0AYY field report).
- CQ presets with accidental leading/trailing whitespace no longer break encoding.

#### ADIF log management on the device

- **Today/All filter** in the log viewer with a **POTA activation counter** — the title
  turns green at 10 QSOs today ("park is open"). Opens on Today; falls back to All when
  today is empty.
- **Single-record delete**: long-press a QSO row, drag to the right line, release →
  Delete/Cancel. For duplicates and botched entries. Deletion keeps the QRZ/eQSL/LoTW
  upload cursors consistent, rebuilds the worked-before cache, and re-mirrors to SD.

#### Display sleep (Samuel W7STF)

- Idle timeout (off/1/2/5/10/30 min) turns the backlight off — everything else (FT8,
  CAT, WiFi, web UI) keeps running. A tap wakes the display (and is swallowed — it
  cannot tune or press anything underneath); a **two-finger double-tap blanks
  immediately**. The big battery win for web-UI-only and unattended use.

#### Settings drawer & Tab5 UI

- **Drawer regrouped**: device/setup items at the top (Flip 180° / Display sleep /
  Battery care / Display brightness / Antenna Tune / WiFi setup / Callsign & Grid /
  Band-plan region), display-tuning controls below with dB Range directly under
  Presets. Frozen drawer header, bigger checkbox hit areas.
- The Antenna Tune slot (1_04+ firmware only) now closes completely on 1_03 firmware —
  no more button-sized hole above WiFi setup — and reopens in place when a 1_04 QMX
  connects.
- Spectrum right-edge scale labels (dBm and flat +dB) stay fully visible — the
  top/bottom tick labels were half hidden at narrow dB ranges.
- dB Range/Alpha drawer sliders now initialise from the stored values (they used to
  show defaults and could clobber each other); waterfall/charge/flip settings included
  in the config export.

#### Web UI

- **Bottom bar decluttered into menus**: QSO Logs (n) / Files / Miscellaneous popup
  menus replace ~11 flat buttons. Tab5 screenshot lives under Miscellaneous.
- LoTW setup pauses the spectrum stream so its one-time 73 KB library fetch cannot
  stall the web server on a weak link; WebSocket sends are capped at 400 ms so a
  congested link can no longer freeze the whole web UI for seconds.
- Flat-spectrum (F) toggle persists across page reloads; battery voltage shown as
  "(8.0V)".

#### Stability

- **USB bulk-error reboot fixed** (IDF patch #4): a transient USB transaction error on
  the CAT pipe could assert-reboot the whole device (`hcd_dwc.c:2406`, serial-captured
  live). The pinned IDF is patched to report a failed transfer instead — the CAT poll
  simply retries. Builders: run `tools/patches/apply_hcd_bulk_error_recovery.ps1` after
  any IDF reinstall (the 4th standing patch script).

#### Notes for testers

- The broken-QSO resume and CQ-run parity hiding shipped after bench verification but
  before extended on-air soak — reports welcome.
- Web-audio streaming (listen to the receiver in your browser — Sam W7STF's request) is
  implemented and working on the bench but deliberately held out of v1.0.0 on a
  development branch pending quality tuning and an overnight soak — it will follow in a
  v1.1.x release.

## v1.0.1 — 2026-07-17 — Pounce report fix

A point release fixing one bug reported within hours of v1.0.0 going out.

- **When answering another station's CQ, the signal report you send back is now your own measurement of their signal** — not an echo of the report they sent you. On v1.0.0, if a station you were receiving at −4 gave you a −10, the reply went out as `R-10` regardless of how strong they actually were; it now correctly sends `R-04`. (Reported independently by Steve N0SZ and Jonathan KN6LFB.) The "Skip TX1" quick-pounce path was already correct — this only affected the normal grid-first pounce. Your received report is still logged correctly as RST_RCVD; only the transmitted report was wrong.

Everything else is identical to v1.0.0.

## v1.1.0 — 2026-07-19 — FT8 decode collapse solved + SD station backup

The headline is a years-old mystery finally root-caused and killed, plus the microSD card promoted from shelved to a full grab-and-go station backup.

**FT8 decode — every slot now decodes like the first (#51).** The long-standing "the first FT8 slots hear 60+ stations, then it collapses to a fraction" behaviour is fixed. Root cause: the USB isochronous audio pipeline queued only **9 ms** of transfers (an untouched driver default), so every post-decode processing burst starved the pipe and **~170–350 ms of the QMX's audio was lost at the USB wire every slot** — with zero error status, invisible to every software counter. That hole clipped the opening sync of every signal, so weak decodes died and the yield sagged; only the very first slots (before any processing burst had happened) were pristine. Fixed by queuing 320 ms of audio. Measured result: **sustained ~16 unique decodes per slot, indefinitely** (previously ~6 steady-state), with no collapse. Belt-and-suspenders hardening shipped alongside (1 s driver audio buffer, higher audio-task priority, a now-visible+counted overflow warning), and FT8 weak-signal decoding was deepened (LDPC iterations restored 15→30 now that a separate timing constraint no longer needs them low) for a further ~⅓ more decodes.

**microSD — full station backup (re-enabled + expanded).** The auto-archive, previously shelved (it was wrongly blamed for the FT8 collapse above — same root cause), is back and now mirrors your **whole station** to the card: the ADIF QSO log, a full config export (settings + memory channels), your **LoTW signing certificate + key**, the diagnostic log, and a self-describing `README.txt`. A genuine PC-free POTA/SOTA backup — pop the card into any computer to back up or move your setup. A plain **FAT32 32 GB** card needs no special handling. (The card holds credentials in clear text, as any restorable backup must — keep it physically secure; the on-card README says so.)

**GPS time sync — automatic, and more precise.** The manual "QMX has GPS" toggle is gone; a GPS-disciplined QMX is now **detected automatically** (by comparing its second-tick against SNTP) and the clock **phase-locks to the GPS second boundary (~10 ms)** rather than a coarse whole-second set. The bottom-bar clock shows `UTC(GPS)` when GPS is the active source, and reflects the source *currently* in charge (`UTC(GPS)/UTC(NTP)/UTC(FT8)/UTC(FT4)`). FT8-derived time sync is now correctly an **offline fallback only** — it's ignored while SNTP/GPS is authoritative (the FT8 slot offset is receive-audio latency, not a clock error). A DT-follow-partner refinement shifts transmit onto a significantly off-time partner's beat during a QSO.

**UI & navigation.**

- **Band-plan drag from the bottom bar** — grab the band-plan slider handle anywhere along the bottom status bar and drag sideways to retune (a much taller target than the thin strip); a vertical up-swipe there still opens memory channels.
- **ADIF log viewer no longer crashes on larger logs** — a real fault when the log grew (~40 QSOs) is fixed by enlarging the LVGL object pool; the viewer now shows ~11 rows and scrolls, with the Close button always on-screen.
- **Settings drawer polish** — equal spacing between the Antenna Tune / WiFi / Callsign buttons (and the FT8-mode dead-gap removed); all sliders align flush-left with the buttons and the knob sits inside the track edge at maximum.
- **FT4 confirmed solid with the SD card mounted** — the tightest-heap mode decodes without collapse.

**Other fixes.** QRZ upload no longer displays "undefined QSOs uploaded" (a display-only count bug). Documentation across the manual + README corrected for all of the above (the microSD "disabled" notes, the removed GPS toggle, the new bottom-bar band-plan drag).

## v1.2.0 — 2026-07-20 — On-device User Manual

**Headline: a built-in User Manual.** The full tab5.lav.dk documentation now reads on the Tab5 itself — open the Settings drawer and tap **User Manual**. It's a dedicated markdown reader (not a web browser — there's no HTML engine on this hardware): it fetches the *same* source markdown that builds the docs website over HTTPS, caches it, and renders it natively with headings, **bold** (shown in colour, since the font has no bold face), lists, tables, code blocks and quotes. A **Contents** page lays the whole guide out in two newspaper-style columns (sections kept intact) with **drag-to-pick** — slide your finger down the list and a highlight bar tracks it; lift to open that page. **Back** walks your page history, **Exit** leaves the manual.

- **Reads the live docs** — one source of truth. Whatever you publish to tab5.lav.dk is what the Tab5 shows; the layout follows the site's nav structure automatically.
- **microSD "Save offline"** — with a card in the slot, one tap mirrors the whole manual to the card so it reads with no internet at all (POTA/SOTA); the button then shows "Saved offline". Reads from WiFi when online, the card when offline.
- **Automatic update check** — the reader checks GitHub for newer firmware releases (pre-releases included) and shows an "update available" banner; a static `latest.json` on tab5.lav.dk is a fallback. It only informs — flashing stays a deliberate act.
- **Docs pipeline** — a small mkdocs build hook publishes the raw `.md` tree + a `toc.json` (from the nav) + `latest.json` alongside the built site, carried by the normal FTP; no second copy of the docs to maintain.

**Also in v1.2.0:**

- **FT8 pileup fix (Dirk DK7CVD):** a station stayed in the pileup list after you'd already worked them — including the nice case where they answer late, cycles after a time-out. The completed call is now dropped from the pileup at QSO completion, and a worked-before check stops a trailing 73/RR73 (or a late reply) from putting them back.

Navigation is otherwise unchanged: the left-edge swipe is still the plain Panadapter ↔ FT8 toggle — the manual is a drawer destination, not part of the swipe stack.

## v1.3.0 — 2026-07-23 — Intelligent Transmit, faster replies, a real practice simulator

This release is built around field feedback from **Roy KI0ER** (groups.io) — smarter manual FT8 operation and faster reply timing — plus a completely overhauled practice simulator, a USB mouse, and a web file browser for the microSD card.

**Intelligent Transmit (the headline).** Tapping a decoded row's **Transmit** now sends the correct *next* message for that station, derived from what they last sent — exactly like a WSJT-X double-click: their CQ → your grid (or your report with Skip-TX1 on, which the manual path now honours too), their grid → your report, their report → `R`+your report, their `R`-report → `RR73`, their `RR73`/`73` → `73`. The report is always **your live measurement of them**, never an echo. **Auto Pounce** appears on any first-reply (including report-first pileup rows); mid-QSO rows get Transmit only. And a fully hand-stepped QSO is now a *real* QSO: sending your manual `RR73`/`73` **logs it to ADIF** (it previously vanished — no log entry, and the partner's trailing 73 haunted the pileup), the partner shows the amber "working" highlight while you step, and they're kept out of your own pileup mid-exchange.

**Reply timing.** The mid-slot reply window widened 2.5 → 2.8 s (the decoder's practical DT ceiling), and a merely-ARMED hand reply now also rides the early-decode cut — so a manual exchange lands on the beat instead of a cycle late. A subtle real bug fell out of testing: a pounce whose first message fired mid-slot kept its "don't scan yet" gate at the *predicted* slot up to 30 s later and discarded the partner's prompt reply — every exchange step repeated once for nothing. Fixed.

**Full cold-pounce — "Fast pounce (early decode)", new toggle, default ON.** Decodes now surface *before* the slot boundary during plain monitoring (WSJT-X style), so answering a fresh CQ can transmit **in the very next slot** instead of waiting a full cycle. ⚠️ **Honest caveat: this specific feature has not yet been A/B-verified on a live band** (no antenna at the development QTH until mid-August). The trade-off is known in principle — capture stops ~1.8 s early, so a *late-transmitting* station's tail can be clipped and its decode lost. If your decodes-per-slot drop noticeably with it ON, **turn it off** (FT8 settings drawer, under "Distance in miles") and please report what you saw — your before/after numbers are exactly the field data we need.

**Pileup / ADIF-log lockout fixed (Roy's report).** While the button shows "Pileup" the on-device ADIF log was unreachable. Now: short-press opens whatever is timely (pileup viewer / log), **long-press always opens the ADIF log**, and a one-time hint teaches the gesture when the button first flips. Pileup behaviour is also now *consistent* with your filters: worked-before stations are excluded from the pileup only when "Exclude worked before" is checked (matching whether the machine would answer them), a just-completed contact's trailing 73 is grace-period-filtered, and **Auto-work pileup** now also starts draining when checked mid-session with callers already waiting (previously only on QSO completion).

**User Manual on a fresh boot.** Opening the manual before WiFi has associated now shows "Waiting for WiFi…" and proceeds when it connects — it no longer demands a reboot.

**FT8 Simulation Mode — now a real practice band.** The simulator was rebuilt end-to-end and **no longer needs the QMX at all** (no radio, no antenna, zero RF — the TX interlock is unchanged):

- **6 phantom stations** (3 US, 3 DX) instead of 2; **4 answer your CQ in the same slot** — a genuine pileup for practicing the pileup tools.
- Phantoms behave like real operators: they **repeat each message every cycle (up to 4×) until you respond**, then give up and go back to CQing; a phantom you're working stops CQing; a worked phantom stops answering your CQs for the session.
- They reply to **whatever you actually transmitted** — auto QSOs, Auto Pounce, and fully manual step-by-step Transmit all get answered correctly.
- The **Fast pounce toggle is honoured in sim**: replies surface just before the boundary with it ON, and after the boundary (with matching slower consumption) when OFF — you can *see* what the toggle does.
- Re-entering FT8 mode clears the phantom decode rows and pileup for a fresh session, and the "connect your QMX" prompt stays hidden while simulating.
- **"Delete test QSOs"** — practice contacts log like real ones (deliberately: it exercises the whole logging path). The ADIF viewer now shows a **"Del N test"** button *only when* simulation records exist (they're recognizable by their missing frequency); two taps wipes them all. Operators who never simulate never see it.

**USB mouse support.** Plug a USB mouse into the Tab5's USB-A port and a cursor appears — clicks drive every menu, button and drawer. **Limitation:** the mouse and the QMX can't share the port simultaneously (a USB hub needs a Transaction Translator the ESP-IDF USB host doesn't implement), so this is for setup, log review, and manual reading with the radio unplugged. Bluetooth mouse support is the eventual path to mouse-while-operating.

**Web microSD file browser.** The web UI's bottom bar gained **Files → SD Files** — browse the microSD card from any computer's browser: download your logs and config, upload files, delete — without pulling the card. All card access respects the same SD/WiFi coexistence discipline as the auto-archive.

**Filter modal layout.** Save / Cancel / Sync Time moved to a tidy stack at the top right; the unattended-TX warning moved out from under the ARRL Field Day row into clear space by the Auto-work pileup option.

**Also fixed en route:** worked-before checks with an unreadable frequency (no CAT) no longer silently pass everyone; the FT8 engine no longer requires a connected QMX just to run its slot loop in simulation.

#### Notes for testers

- **Fast pounce (cold-pounce early decode) ships on-air-untested** — see the caveat above. The toggle is your safety valve; reports with decode-per-slot numbers (ON vs OFF, same band/time) are gold.
- A single unexplained heap-assert reboot (`tlsf_free` double-free, ~28 s after boot) was observed **once** during this release's bench testing and never reproduced. If your Tab5 spontaneously reboots shortly after power-on, please grab the saved diagnostic log (web UI → "Diag(saved) ↓") and report it.

## v1.3.1 — 2026-07-24 — DT + HZ columns, UI polish, diagnostics fix

A small follow-up release: two decode-list columns requested by Roy KI0ER the day v1.3.0 shipped, a batch of visual alignment work, and a diagnostics fix reported (with root-cause analysis) by Paul VE3PIK.

**Decode list: DT and HZ columns (Roy KI0ER).** Two new columns after SNR:

- **DT** — the station's slot-timing offset in seconds, shown relative to the band as a whole, so an on-time station reads ~0.0 and an off-time one shows its true offset (the same at-a-glance number WSJT-X users watch).
- **HZ** — the station's audio tone, i.e. where it sits in the FT8 passband. (Your own transmit tone is shown in the TX confirmation dialog, and the firmware picks a clear tone automatically.)

The width comes from the country column: full entity names are replaced by **3-letter codes** (ISO alpha-3 where the entity has one — USA, DEU, JPN — and a recognizable tag for ham-only entities like HAW or SAR). SL, CALL and MESSAGE keep their positions; KM/MI, BRG and HRD remain.

**Diagnostics: the boot log's "TAB5 BSP INFO" block now reports the hardware that was actually detected** (Paul VE3PIK, with the fix approach). It used to identify the touch controller by a bare I2C address probe — but ST7121 and ST7123 share the same address, so the block said "ST7123" on every ST7121 unit, contradicting the driver's own correct detection two lines earlier in the same log. It now reports the driver's cached detection result (and the older GT911 variant's panel is named ST7703, consistently with the rest of the firmware).

**UI polish:**

- The breathing "Now turn on or reboot your QMX/+" prompt no longer overlaps the FT8 screen's left pane (position is now mode-aware: offset right in FT8, centered in Panadapter).
- FT8 left pane aligned to a uniform grid: all boxes share the same right edge, equal 8 px spacing between the TX/Filter/Call-CQ buttons, and the same corner radius throughout.
- Settings drawer cleanup: dropdowns are sized to their content (no more full-width white bars) with a light-grey background, slider tracks are half as thick (knob unchanged), and the Display section header is gone — the label now reads "Display brightness".

#### Notes for testers

- The v1.3.0 notes still apply: **Fast pounce (early decode)** remains on-air-unverified — ON/OFF decodes-per-slot comparisons are welcome — and the one-off boot-time reboot is still being watched for.

## v1.3.2 — 2026-07-26 — PSK Reporter, grid-square fix, the manual built in

Three things worth your attention in this one: your QSOs get their grid squares back, the Tab5 now contributes reception reports to PSK Reporter (**on by default** — see below), and the user manual is built into the firmware so it works with no WiFi and no SD card at all.

### Grid squares are logged again (John W5JSS)

**Almost every logged QSO was missing its `GRIDSQUARE` field.** John measured 5 of 60 on v1.0.1, and it was unchanged through v1.3.1. The decode table stored each station's grid correctly from their CQ, then wiped it again on the *next* message from that station — because a report, `R`-report, `RR73` or `73` carries no grid, and the code overwrote the stored grid with the empty one instead of keeping what it had. By the time a QSO completed and the log entry was built, the grid was gone.

Confirmed on the operator's own log: 5 of 34 records had a grid before the fix, and both test QSOs after it logged theirs correctly. This affects the ADIF log and everything built from it — LoTW, QRZ and eQSL uploads, and the decode list's KM/MI and BRG columns. **Previously logged QSOs are not retro-fixed**; new ones are correct.

### PSK Reporter spotting — ON by default

The Tab5 now reports the stations it decodes to [PSK Reporter](https://pskreporter.info), the same as WSJT-X does, so you appear on the map as a monitoring station and other operators can see where they were heard.

**What is sent:** your callsign and grid square, plus for each station you decode — their callsign, their grid (if it was in the message), the frequency, the signal report, and the mode. Batched and sent at most once every five minutes. **Nothing is transmitted on the air**; this is an internet report only.

**To turn it off:** FT8 settings drawer → uncheck **"Report to PSK Reporter"**. It also does nothing at all unless your callsign *and* grid are set, and it is disabled outright in simulation mode so practice contacts never reach the public map.

Two problems were found and fixed before release, both of which would have been invisible in normal use because PSK Reporter never acknowledges anything:

- A callsign the decoder could not fully resolve (shown as `...` in the decode list) would have been published to the public database as a literal callsign. It is now rejected, as are over-long callsigns that would otherwise have been truncated into somebody else's call.
- The report packet violated the IPFIX padding rule in a way that depended on the *lengths* of your callsign, grid and firmware version — so roughly a quarter of stations would have had their reports silently ignored or mis-parsed, and the rest would have worked fine. Found by decoding the bytes the hardware actually emits with an independent parser (`test/psk_harness.c`), not by reading the code.

**Confirmed working in the field (2026-07-27):** "QMX Panadapter v1.3.2" now appears under "Software in use" at `pskreporter.info/cgi-bin/pskstats.pl`, with reports arriving from six stations — BD4AHS, KI0ER, VE3OFA, W5JSS, W5NR and W7STF. The collector accepts our reports; nothing further is outstanding here. FT4 spots are reported as well as FT8.

Two things worth knowing if yours seems not to work. **The first report is sent 5–5½ minutes after switching on**, so a short trial shows nothing and looks like a failure. And PSK Reporter never acknowledges a report, so the "Software in use" page is the only place the truth shows — third-party dashboards built on top of PSK Reporter do not always display spots that the collector did in fact accept, which is what led one operator to think reporting was broken when his spots were arriving normally.

### The user manual is now built into the firmware

Settings drawer → **User Manual** works immediately, always: no WiFi, no SD card, no download, no waiting, on the very first boot. The whole manual ships inside the firmware image (~136 KB), so it also can never describe a different version than the one you are running.

The green **"Save offline"** button is **gone**, along with downloading the manual and copying it to the SD card — none of which is needed any more, and all of which could fail. If you had previously saved the manual to a card, that copy is simply ignored; you can delete `/qmx-panadapter/manual/` from the card if you want the space back.

### microSD backup: what gets written, and when

Behaviour here is now explicit rather than best-effort, because on this hardware the SD card and WiFi cannot reliably share the bus:

- **WiFi off** (the POTA/SOTA case) — continuous mirroring, exactly as before. Verified running clean for the full length of a soak with no errors.
- **WiFi on** — one complete backup per start-up. Your QSO log, config, and LoTW certificate and key are all on the card within about five seconds of switching on; QSOs made later in that session reach the card at the next start-up.

The bottom-bar **SD dot** now has two states: **green** while mirroring continuously, **yellow** once the start-up backup is done and mirroring has stopped. The card's own `README.txt` explains this too.

**Insert the card before switching on.** A card pushed in later is not picked up until the next start-up.

This also removed a real hazard: previously the card was torn down at an unpredictable moment mid-write, with the log file still open — which is exactly how FAT directory entries get corrupted. The Tab5 now closes its files and steps back deliberately instead.

### FT8 operating

**Grey-listing for stations that never answer (Roy KI0ER, opt-in).** Enable **"Allow grey-listing"** in the FT8 Filter modal and any station that times out two pounces in a row is set aside: the robot and Auto-work-pileup skip it, its row turns violet, and tapping it offers to clear it rather than opening the TX dialog. Off by default; the list is forgotten at power-off.

**Pileup replies now use the same laddering as a manual Transmit.** Working a caller from the pileup builds the correct *next* message from what that station actually last sent, instead of always starting with a signal report. That fixes the lost comeback where a station answers minutes later with a report and needs `R`+report — which the old fixed path could never produce.

**The auto-answer robot now works in simulation with no QMX attached.** It was only ever ticked after a successful audio capture, so with no radio present it silently did nothing.

### Smaller fixes

- **Update check** now asks `tab5.lav.dk` before GitHub and retries once on a transient failure, instead of giving up and reporting no version information. (The site request doubles as an anonymous count of active devices — it is a plain file fetch with no identifying data.)
- **Reader:** a ≥3 s hold on the User Manual button clears the reader's caches; the header shows status inline rather than over the text.
- **UI:** settings-drawer section heights reflow correctly; the FT8 screen gained a bottom separator line.
- Boot log now reports the built-in manual's entry count and size, so a mis-packed manual is visible in a diagnostic log instead of silently showing "page not found".

#### Notes for testers

- **PSK Reporter is the one thing that genuinely needs a field check** — see above. Remember the 5½-minute delay before the first report.
- **Fast pounce (early decode)** from v1.3.0 is *still* on-air-unverified. ON/OFF decodes-per-slot comparisons on the same band and time remain the most useful report anyone can send.
- Live SD mirroring during a WiFi session is a known limitation, not a bug to report: the underlying cause is the SD and WiFi sharing a DMA controller, which is not fixed in this release. The start-up backup is unaffected.
- The one-off `tlsf_free` boot-time reboot from v1.3.0 was not seen again this cycle and is still being watched.

## v1.3.3 — 2026-07-29 — Your TX frequency, on screen and under your control

This release is almost entirely Roy KI0ER's field feedback, plus a volume control for control-panel-less QMX+ builds and a LoTW gap that was quietly costing US operators award credit.

### You can see and change your TX audio frequency (Roy KI0ER)

Until now the Tab5 picked your transmit tone for you, silently, and never told you what it chose. There was no way to read it and no way to move it. Two things follow from that: you could not tell whether you were sitting on top of somebody, and when you were, you could not get out of the way.

Both are fixed.

**Reading it.** The tone now has its own line in the TX status block, in both the "TX armed" and "Transmitting" states. The `FREQ BUSY` warning names the frequency too, so it tells you something you can act on.

**Changing it.** A new **TX nnnn Hz** button appears on the FT8 screen next to "Active: N" whenever a CQ or QSO is running. Tapping it opens a tone picker:

- A **live occupancy strip** across the whole 200-2800 Hz audio window. Green is free, red is occupied, amber is you, cyan is your QSO partner.
- **Touch and drag** along the strip to pick a slot. The bar you are holding turns grey and follows your finger, the big readout tracks it live, and it commits when you lift off.
- **-50 / +50** buttons for a nudge, and a **Find clear slot** button that scans outward from wherever you are.
- The free slots are also spelled out as numbers underneath, and the readout says in words whether the slot you have chosen is clear or occupied.
- **Apply nnnn Hz** commits it to the radio.

It applies **between** bursts and refuses while a burst is on the air, which is what Roy asked for. Slot parity is untouched, so your partner keeps tracking the exchange exactly as WSJT-X users expect when they retune mid-QSO.

One honest limitation: the strip shows **decoded stations** and their guard bands, not raw spectrum. A station too weak to decode will not show up as occupied, and grey across the whole strip means nothing has been heard yet rather than "the band is empty".

### It no longer calls a station that is busy with someone else (Roy KI0ER)

On a crowded band several people answer the same CQ and the caller picks one of them. When that was not you, the Tab5 kept calling anyway — six full-power transmissions at a station whose own decode row plainly showed it mid-exchange with somebody else — and then gave up.

Now it waits. While their last message is addressed to a third party, nothing is transmitted. When they sign off with `73` or `RR73`, or call CQ again, it picks straight back up.

The part that matters most is not the wasted battery. Giving up on a station used to **grey-list** it after two attempts, so a perfectly good, perfectly audible station could end up permanently skipped by the robot and Auto-work-pileup for no reason other than being popular. A hold no longer counts as a failed attempt, so that cannot happen. The wait is capped at about six minutes, so a station that simply disappears mid-exchange still times out normally.

### An incoming "RRR" now finishes the QSO (Roy KI0ER)

`RRR` is the older form of `RR73` and means the same thing at that point in an exchange. The Tab5 did not recognise it, so when Roy worked NH6L the two of them deadlocked — NH6L sending `KI0ER NH6L RRR` every slot, the Tab5 answering with the same signal report every slot, and the QSO only completing when Roy stepped in by hand. An `RRR` now closes the exchange exactly like `RR73`, and the next thing sent is `73`.

### QMX volume from the Tab5 (Randy N4OPI)

Randy runs a QMX+ with no control panel, which means no volume knob at all. There is now a **QMX volume** slider in the settings drawer, directly under Flip 180, in both Panadapter and FT8 modes.

The value is **in decibels — the same number the radio shows on its own LCD**, not an invented percentage. It also reads the radio back each time the drawer opens, so if you change the volume from the rig the slider follows rather than disagreeing with it.

### LoTW: US state and county (found via Paul N8HM)

Paul N8HM pointed out that ARRL's LoTW documentation is out of date and that TQSL's own source is the thing to work from — which is how this implementation was built — and offered his CardSat project as a cross-check. Comparing the two turned up a real gap on this side: **the Tab5 was not sending station state or county at all.**

For a US operator that means uploaded QSOs earned **no Worked All States and no county credit**, for them or for the stations they worked. There are now two fields for it in the LoTW setup page. Fill them in if your TQSL station location has them; the county is the name on its own, not `ST,Name`.

Two details worth knowing:

- Adding them does **not** re-upload your log. The upload position now only resets when the certificate itself actually changes, so a two-field edit no longer re-sends everything.
- Everyone outside the US is unaffected, and existing signatures are unchanged.

Verified against the worked example in CardSat's own format notes, which came from a QSO LoTW accepted and posted — so the field ordering is checked against something external, not just against itself.

### Fixes

- **The "decode my partner's reply first" optimisation had been aimed at the wrong frequency since v0.18.4.** It was pointed at *our own* transmit tone instead of the partner's. Because a pounce deliberately transmits on a clear slot away from the station being called, those are two different frequencies — measured here as 250 Hz and 750 Hz apart on two test QSOs — so the optimisation never did anything. It exists to stop a busy band pushing a reply past the immediate-reply window, which was the original complaint behind it.
- **The decode list is cleared when you leave simulation mode**, so phantom stations no longer linger in a list that is supposed to be real — and no longer sit there tappable.
- **The decode list and pileup are cleared when you switch between FT8 and FT4.** The two modes use different slot lengths, so rows decoded under the other timing describe a band the new mode cannot hear.
- **The practice simulator was silently broken with "Fast pounce" turned off** — which is the default. Phantom stations never replied at all, so every practice pounce timed out. A reply was scheduled one second later than the retry that reset it, and it lost that race every slot, forever. Nothing logged the miss; it simply looked as though the phantoms were ignoring you.
- **LoTW upload success detection** now matches TQSL's own test exactly (a substring check rather than an exact one), so a decorated-but-successful status can no longer be read as a failure.

### PSK Reporter is confirmed working

The one item v1.3.2 flagged as needing a field check has been answered. `QMX Panadapter v1.3.2` is listed under "Software in use" at [pskreporter.info](https://pskreporter.info/cgi-bin/pskstats.pl), with reports arriving from six stations — BD4AHS, KI0ER, VE3OFA, W5JSS, W5NR and W7STF. FT4 is reported as well as FT8.

Two things to know if yours looks dead: the first report goes out **five to five and a half minutes** after switching on, so a short test shows nothing; and PSK Reporter never acknowledges a report, so that page is the only place the truth shows. Third-party sites built on PSK Reporter data do not always display spots that were in fact accepted — which is exactly what led Samuel W7STF to think his reporting was broken while his spots were arriving normally.

#### Notes for testers

- **The QMX volume slider has not been tested against a radio.** No QMX was attached while it was written. The command format, the value read back from the radio, and whether the dB figure on the Tab5 matches the dB on the QMX LCD all want confirming. If the slider does nothing, or the useful range is squeezed into one end of its travel, that is the thing to report.
- **The tone picker has not been used on a crowded band.** It was verified against the practice simulator, which puts six stations at fixed frequencies — nothing like a real evening on 20 m. Reports on whether the occupancy strip matches what you can actually hear are welcome.
- **The busy-station hold has the same caveat**: the simulator cannot produce a real pileup.
- **The LoTW state/county path needs a US callsign certificate to exercise.** It is written so the fields are optional and never required, precisely because that could not be checked here.
- **Fast pounce (early decode)** from v1.3.0 remains on-air-unverified. ON/OFF decode counts on the same band and time are still the most useful report anyone can send.
- Live SD mirroring during a WiFi session is still a known limitation rather than a bug: the SD card and WiFi share a DMA controller. The start-up backup is unaffected.
- The one-off `tlsf_free` boot-time reboot first seen in v1.3.0 has still not recurred and is still being watched.

## v1.3.4 — 2026-07-29 — Finishing the QSO, and a TX frequency you control from the main screen

v1.3.3 went out in the morning and the reports came back the same day. This release is those reports: two real bugs in how QSOs finish, an invented signal report that should never have been in the log, a volume slider that worked but was unusable, and an occupancy map that was answering the wrong question. Plus the TX frequency control moved out of a corner and onto the main screen, with a live band picture beside it.

### A QSO is not over just because we said 73 (Roy KI0ER)

Roy worked VE3INB with auto-reply running. VE3INB sent his report, the Tab5 logged the QSO as complete and moved on to the next CQ — but VE3INB had never decoded the final `R73`, so he kept sending `KI0ER VE3INB R-10`, slot after slot, waiting for the one message that would let him close his own log. He never got it. Roy stepped in by hand and sent it himself, which produced a **second copy of the same QSO** in his ADIF log. He did it again. More copies. Meanwhile VE3INB gave up, and Roy is probably not in his log at all.

Two separate faults, and the second explains the first.

**The Tab5 now answers a partner who is still asking.** Nothing in the machine could act on VE3INB's repeated report. The record kept for resuming a broken QSO is deliberately erased on completion — that is what stops a fading partner being auto-resumed into a duplicate — and a completed QSO is otherwise finished, full stop. So the slot went to the next CQ while the previous contact was still unfinished on the other side.

If the station just worked comes back addressing us with a report instead of `RR73`, `73` or `RRR` — meaning they did not hear our final — the final goes out again, up to three times within four minutes. That check runs *before* anything can start a new contact, so the slot is spent finishing the previous QSO rather than beginning the next one. Nothing is logged a second time, and an armed CQ resumes by itself afterwards. This is what WSJT-X does, and it is what actually gets you into the other operator's log.

**And it will not log the same contact twice.** The log is written at exactly one moment, when the final leaves the air. Taking over by hand legitimately drives the machine through that moment again, which is where Roy's duplicates came from. The same callsign on the same band within ten minutes is now recognised as the same contact and logged once.

### Your log no longer contains signal reports nobody sent (Roy KI0ER)

Roy found `599` in the received-report column of his log and asked, reasonably, whether FT8 stations really were sending him 599.

They were not. That was ours. When a QSO finished without a numeric report ever being exchanged — the partner answers your grid and jumps straight to `RR73`, for instance — the ADIF `RST_SENT`/`RST_RCVD` field was filled in with `599`. In a CW or SSB log that is a harmless convention. In an FT8 log it is a **fabricated measurement**, and it was being uploaded to QRZ, eQSL and LoTW as though it had been measured.

Unknown reports are now left out of the record entirely. ADIF requires neither field and LoTW ignores both. The on-device log viewer shows a dash. Records already written keep their `599` — the log is not rewritten, since it may already have been uploaded.

### The occupancy map now answers the right question (Roy KI0ER)

Roy asked whether the new occupancy strip accounts for *which time window* you transmit in — because a slot occupied in the odd window may be completely free in the even one, and the answer decides whether it is a candidate for you.

It did not. It counted every decoded station regardless of the slot it was heard in, so a tone busy only in the *opposite* window showed as busy for you. On a crowded band that is most of the strip, and the automatic clear-slot picker was making the same mistake — steering away from slots that were in fact free.

Occupancy is now filtered by slot parity: two stations only collide if they transmit in the same window. One honest limit, because it is worth knowing rather than guessing: your own transmit window is only knowable once something is armed or running (a reply inherits the opposite of your partner's, and a CQ carries your `TXCQ EVEN`/`ODD` choice). With `TXCQ ANY` and nothing armed, there is no answer to give, so the map falls back to showing both windows combined — the same conservative view as before. When it does know, the free-slot line under the strip names the window it is showing.

### The QMX volume slider is usable now — and it works (Randy N4OPI)

Randy is the first person to have run the v1.3.3 volume slider against a real radio, and the news is good: the command format, the value read back from the rig, and the live dB on the LCD all work as intended. That was the release's largest untested claim.

His complaint was the travel. The slider covered the QMX's full protocol range of 0 to 199 dB, but everything usable sits inside the first ten percent of it — "anything beyond that is way too loud" — so every setting an operator actually wants was crammed into the leftmost couple of centimetres.

The slider now tops out at **40 dB**, double the highest useful value reported, so nothing reachable has been lost and there is five times the resolution where it matters. If you turn the radio past 40 dB with its own knob, the slider knob sits at the end of its travel but **the number still shows the radio's true dB** — the figure has to agree with the LCD, which is the whole point of the control.

Randy also noticed the QMX remembers volume per band. That is the radio's own behaviour, not the Tab5's.

### The TX frequency is a control on the main screen, with the band beside it

In v1.3.3 the transmit tone was a small chip that appeared next to "Active: N" only while a CQ or QSO was running — which is to say it was hidden at exactly the moment you were deciding where to transmit. It is now a permanent, full-height button in the FT8 left pane, always showing a number, defaulting to the conventional 1500 Hz.

- **A live occupancy strip sits under the slot countdown.** The same 50 Hz grid, the same occupancy data and the same colours as the picker's full-size strip, shrunk to the width of the left pane. Where the band is busy — and where you are in it — is now answerable at a glance without opening anything.
- **`TX Hold`, WSJT-X's "Hold Tx Freq".** A checkbox in the tone picker. With it on, the tone you chose is the tone used for every CQ and every reply, and a clash is reported but never acted on. With it off, the behaviour is as before: each transmission takes the nearest clear slot. A line under the checkbox states which of the two you are getting, in words. Both the tone and the hold setting survive a power cycle.
- **The parity preference is one button instead of two.** `TX: EVEN` and `TX: ODD` each toggled themselves off again, spending two cells of the pane on a three-way choice and leaving "any" as an implied state you reached by un-picking something. It is now a single button cycling `TXCQ ANY` → `TXCQ EVEN` → `TXCQ ODD`, keeping the colours the pair had.
- **`Active: N` is gone.** It was not useful information, and the status text below it — which wraps, and changes several times a slot — took over the line.
- **The tone is no longer repeated on the TX status line.** The button above shows it at all times and is also what moves it, so the copy was pure duplication, and it cost a line on the label that can least afford one.

In the picker itself: your own tone is now **white** and your partner's **pink** (against green and red occupancy bars, the old amber read as a warning and cyan as a third state, when both are really just "mine" and "theirs"), `Apply` is the same green as every other commit button in the app, the ±50 nudges and the clear/busy verdict are larger, and the layout is centred and better spaced.

### Bottom bar: WiFi signal strength as an icon

The `-NN dBm` figure is gone from the bottom bar. WiFi strength is now shown the way every phone and laptop shows it — a **fan icon** whose lit elements track the link: the dot alone above 25 %, plus the first bow above 50 %, plus the second above 80 %. All three stay faintly visible so the icon never changes width and the count reads against a whole.

That freed a useful amount of width, and it goes to the **SSID**, which is frequently long and was being truncated. The IP address is pinned to the right edge, the SSID sits beside it, and the icon follows the text. The centred UTC clock does not move: if an SSID is long enough to reach it, the SSID truncates rather than the clock shifting.

### Under the hood

**The settings dirty-bitmap was full, and is not any more.** Every one of its 64 bits had been allocated — the last went to the QMX volume slider in v1.3.3 — so the next setting that wanted to be remembered across a power cycle simply could not be. It is now a 128-bit set with 62 bits spare, and grows by changing one number. The `TX Hold` tone and flag are the first two settings to use the new room. There is no migration: nothing in flash depended on the old layout.

#### Notes for testers

- **The QSO-completion re-send is bench- and simulator-verified only.** Whether three re-sends inside four minutes is the right budget is a judgement about on-air conduct that only real contacts can settle. If a station needs more persistence than that, or if you see the Tab5 being *too* persistent, that is the report to send.
- **The parity-aware occupancy map has not been checked against a real crowded band.** The logic is right — stations in the other window cannot collide with you — but whether the strip now matches what you can actually hear is worth a look.
- **The QMX volume dB figure has still not been compared against a QMX LCD side by side.** Randy has confirmed the slider works and moves the volume; whether the two numbers read identically is the outstanding half.
- **Fast pounce (early decode)** from v1.3.0 remains on-air-unverified. ON/OFF decode counts on the same band at the same time of day are still the most useful measurement anyone can send.
- The **LoTW state/county** path still needs a US callsign certificate to exercise.
- Live SD mirroring during a WiFi session remains a known limitation rather than a bug: the card and WiFi share a DMA controller. The start-up backup is unaffected.
- The one-off `tlsf_free` boot-time reboot first seen in v1.3.0 has still not recurred and is still being watched.

## v1.3.5 — 2026-07-31 — Managing the log, pacing the CQ, and a way out of the wait

Another same-week feedback release. Don WB0LQW asked for three things — a way to clear the log from the Tab5, a way to work with the log without downloading it, and a CQ that stops calling after a few tries. All three are here. Roy KI0ER found that the v1.3.4 busy-station hold had no exit. And the settings drawer's touch handling got the fix that was promised on groups.io.

### Send CQ a few times, then pause (Don WB0LQW)

Don: "I usually send CQ 2-4 times and then pause... it would be nice if there was a counter, or a limit I could set."

Both now exist. Long-press **Call CQ** and the preset editor has a **CQ stop** button at the top right, cycling never / 1 / 2 / 3 / 4 / 5 / 10 calls — it applies the moment you tap it, no Save needed. While calling, the TX status shows the counter live: "call 2 of 4" (or just "call 2" with no limit set).

The stop itself is polite about timing: after the last unanswered call, the Tab5 listens through one more receive slot — an answer to your final call still starts the QSO normally — and only then stops and goes idle. The limit applies to every CQ run, including the automatic resume after a completed or timed-out contact, and each fresh sequence starts the count over. The setting survives a power cycle and travels in the config backup.

### The QSO log, in your browser (Dennis WN4FLA, Don WB0LQW)

Both Dennis and Don went looking for log management in the web interface and found only the download link — and Don had been told, wrongly, that a whole-log clear was already there. The clear function existed inside the firmware; no button had ever been wired to it. That is owned up to and fixed properly.

The QSO Logs menu now has **View / edit log**: the whole log as a table in the browser — call, mode, band, frequency, date, time, both reports, grid — newest first. **Click any column header to sort** by it, click again to reverse; sorting by date groups an activation's QSOs together, which was the practical need behind the "today-only download" request. Every row has a delete cross for removing that one record, and a **Delete all** button clears the whole log — that one asks you to type `DELETE`, because there is no undo.

Single-record deletion from the browser is deliberately paranoid: the delete request carries both the record's position and its callsign, and the Tab5 refuses if they no longer match — so a log that changed since the page loaded (a new QSO mid-view) can never cause the wrong record to be deleted.

### Delete all, on the Tab5 too (Don WB0LQW)

The on-device ADIF log viewer gets its own **Delete all** button, next to Close. Same two-tap confirm as the test-record delete: the first tap arms it and the label changes to "ALL 34?" with the live count, a second tap within five seconds deletes, waiting disarms it.

This is the POTA workflow Don described: clear the log at the start of the activation, and the ADIF file at the end is exactly what you submit — no editing, no filtering.

### A bug the missing button was hiding

Clearing the log never reset the QRZ, eQSL and LoTW upload positions. Since nothing could reach the clear function, nothing ever hit the bug — but the first operator to clear a 30-QSO log would have found their next 30 QSOs silently skipped by every upload, because the upload cursors still pointed past them. Fixed before it bit anyone: a cleared log now resets all three.

### You can now escape the busy-station hold (Roy KI0ER)

v1.3.3 taught the pounce to wait politely when its target is visibly working someone else, instead of keying up over their exchange. Roy found the flaw the same week it shipped: the wait had no exit. The hold deliberately disarms the transmitter, and the cancel action only existed while a transmission was armed or on the air — so the one state where you most want to opt out was the one state without a way to. You were committed to a station you were not actually working, locked out of pouncing on anyone else.

The status line now shows **TAP TO CANCEL** during the hold, in the same amber as an armed transmission, and tapping it drops the pounce so you are free to work anyone else. The abandoned exchange stays resumable for a few minutes in case the station frees up and you want back in. The same escape covers the CQ auto-stop's final listening slot.

### The settings drawer takes your finger seriously now (Don WB0LQW)

Drawer sliders and checkboxes responded intermittently — whether a slider moved came down to how still your finger was. Not a hit-target problem: LVGL hands the gesture to the scrollable drawer the moment a touch travels ten pixels, and ten pixels is nothing for a fingertip on this panel. A grabbed control now stays grabbed (the drawer no longer steals the gesture mid-drag), and the slider knobs' catch area more than doubled to 98 px after a bench test showed the first attempt was still too tight. The stated cost: a drag starting exactly on a knob or checkbox no longer scrolls the drawer.

One known leftover, deliberate: you still need to touch near the knob *horizontally* — a tap at the far end of the track does nothing. Making the whole track grab would let a stray tap jump the volume by 30 dB, so it stays knob-only.

### QMX volume: 50 dB, and the number is verified (Randy N4OPI)

Randy — still the only person who has measured this against a real radio — compared the Tab5's dB figure against the QMX's own LCD side by side: **they read identical**. Nothing in the volume path is unverified any more, closing v1.3.4's last outstanding claim about it.

The cap moved once more, from 40 to **50 dB**, on his third and best-informed report: his earlier "40 is not quite loud enough" turned out to have been measured with the antenna switched off, so what was missing was band noise, not gain range. With the antenna on, "40 seems plenty loud now. Maybe 50?" — so 50, with about 10 px of slider travel per dB. As before, turning the radio's own knob past the cap pins the slider knob at the end while the number keeps showing the radio's true dB.

#### Notes for testers

- **The CQ auto-stop's on-air pacing is bench-verified only.** The counter, the setting and the stop logic all work on the bench; that it stops after exactly the Nth unanswered call on a real band, and resumes cleanly on the next Call CQ, is Don's to confirm.
- **The busy-hold TAP TO CANCEL needs a live band to exercise** — the hold only engages when a pounce target is visibly working someone else.
- The **QSO-completion re-send budget** (three re-sends inside four minutes, v1.3.4) is still bench-verified only.
- **Fast pounce (early decode)** from v1.3.0 remains on-air-unverified.
- The **LoTW state/county** path still needs a US callsign certificate to exercise.
- Live SD mirroring during a WiFi session remains a known limitation rather than a bug: the card and WiFi share a DMA controller. The start-up backup is unaffected.

---

## v1.3.6 (2026-08-03) — the USB reconnect saga solved, a field crash fixed, and WiFi scan that works away from home

A pure fixes release — no new operating features, but two of the fixes close problems that have been part of living with the panadapter since the beginning. Everything below was found, reproduced and verified in a single marathon bench session with a serial monitor attached (Hoi An, with the QMX on firmware 1_04_004 by the end of the night).

**The "restart the QMX so many times" mystery — resolved into two separate bugs, both now handled.**

- **The Tab5-side wedge ("zombie device") — FIXED.** Powering the QMX off while it was streaming could fail the USB teardown inside the audio driver, leaving a dead device object permanently occupying the USB port: the QMX became invisible no matter how many times it was restarted, and only a Tab5 reboot recovered. Root-fixed in our USB-audio driver fork (the teardown now always completes), with a belt-and-suspenders automatic recovery on top: if a device sits unrecognized on the port, the Tab5 now electrically "replugs" the port by itself (verified live — twice — including once fired remotely through the wedged unit's own web server). Net effect: QMX off → on now reconnects in about a second, hands off.
- **The QMX-side wedge — detected and explained on screen; the fix belongs to QRP Labs.** After some Tab5 restarts, the QMX answers USB enumeration with a truncated device descriptor (8 of 16 requested bytes), forever — through bus resets, port power cycles, VBUS cuts and even physical cable replugs. Only restarting the QMX clears it. Reproduced identically on QMX firmware 1_03_002 and 1_04_004 and reported to QRP Labs. The Tab5 now recognizes the state and shows **"QMX USB is stuck - power-cycle the QMX to reconnect"** instead of sitting on a dead-looking screen.

**FT8 crash on radio power-on — FIXED (Dennis WN4FLA).** Turning the radio on while the Tab5 sat waiting in FT8 mode (or changing bands around that moment) could reboot the Tab5 — Dennis hit it three times in one morning. Reproduced on the bench with his exact steps on the first try, with the serial backtrace the field could never provide: the FT8 engine's periodic restart could overlap its own shutdown and tear shared queues out from under a live decoder task. The engine's internal communications are now owned per-instance, the shutdown waits are ordered correctly, and a new engine refuses to start until the old one is fully gone. Torture-tested with every radio-off/on and band-change combination we could invent: no crash.

**WiFi scan now works where it matters (hotel/POTA).** The WiFi setup's Scan button always returned "No networks found" whenever the stored network was unreachable — precisely the situation Scan exists for. Root cause: the automatic reconnect loop both starved the scan of radio airtime and (the killer) `esp_wifi_connect()` flushes the scan results before they could be read. Fixed with a scan-hold on the reconnect chain, harvest-before-reconnect ordering, a fix for scans started during the reconnect backoff, plus an automatic retry and proper UI timeouts. Verified five-for-five on hotel WiFi.

**Web UI: live TX status banner (Dennis WN4FLA).** In FT8/FT4 mode the web page now shows the same status as the Tab5's own TX label — red while transmitting (with the "call 2 of 4" CQ counter), amber armed/waiting, green QSO complete, orange timeout, and the persistent "CQ stopped after N calls - no answer" from the v1.3.5 auto-stop. The browser tab's title carries a red dot while transmitting, so even an unfocused tab signals from across the room.

**"Diag(saved)" download fixed.** With an SD card inserted and WiFi on, the saved-diagnostics download served an empty placeholder or a stale months-old card snapshot instead of the fresh crash log — hiding exactly the data it exists to deliver (found via Dennis's empty post-crash download). It now always serves the flash-persisted copy, including the rotated older half so rotation can't hide a crash lead-up.

**For builders:** two new standing patches this release — `tools/patches/apply_hub_recover_tolerant.ps1` (IDF patch #5: the hub driver's root-port recovery abort → tolerant, required for the USB auto-recovery to be crash-safe) joins the four existing apply-scripts, and the USB-audio fork (`components/espressif__usb_host_uac`, in-repo) carries its 4th patch (forced teardown on a dead device).

**Still on-air-unverified (carried over):** the v1.3.5 CQ auto-stop pacing and busy-hold cancel, the v1.3.4 final re-send budget, and v1.3.0 Fast pounce.

---

### Shipped in v1.4.0 — 2026-08-05

**Live spots on the spectrum, and three long-standing instability causes root-caused.**

**Live spots (POTA + RBN).** `main/net/spots.c` fetches `api.pota.app/spot/activator` (~95 spots, ~40 KB) about once a minute into a mutex-protected PSRAM store; `main/net/rbn.c` adds the Reverse Beacon Network telnet feed as a second producer into the SAME store, so the display never knows the source. Drawn by `main/ui/spots_lane.c` as a **see-through overlay on the spectrum** (FlexRadio convention): bright callsign centred on the spectrum's middle, a 2 px line dropping to the frequency axis, amber POTA / green RBN / **grey = already worked on this band**. Opacity fades with age and the spot disappears at 30 minutes. Band-scoped off-screen counters in the bottom corners, tappable, coloured like the spot they lead to. **Press-drag-lift** selection: press a callsign, drag to re-snap to the nearest, lift to tune — frequency **and** mode (CW / DiGi / USB above 10 MHz / LSB below; unknown mode leaves the radio alone). Bandwidth deliberately not forced — the QMX reloads its own per-mode filter on a mode change. Drawer section "Live spots (POTA)" + indented "Add RBN (CW skimmers)"; `spots_en` default ON, `rbn_en` default OFF (a continuous feed on this board's most delicate subsystem); both in the config export. RBN is band-filtered at ingest, deduplicated ~4.75:1, and holds a station for 10 minutes.

**ALL outbound HTTPS was dead — fixed.** Every TLS attempt failed at RNG seeding: `esp-aes: Failed to allocate memory for the array of DMA descriptors` → `mbedtls_ctr_drbg_seed returned -0x0001`, taking QRZ, eQSL, LoTW and the update check with it. On ESP32-P4 `SOC_AES_SUPPORT_DMA=1` and the IDF AES port has no small-buffer fallback, so even ctr_drbg's 16-byte operations allocate a descriptor array from `MALLOC_CAP_DMA|INTERNAL` — impossible with the pool at ~200 B. Diagnosed by measurement (a probe reproducing the exact allocation: succeeds pre-WiFi, fails after), not by adopting the standing theory. Fixed by moving AES/GCM/SHA to software; **MPI and ECC stay hardware**, so RSA/ECDSA — the expensive part of a handshake — is still accelerated. Also fixed: `update_check` was the only network path not pausing the spectrum WebSocket during its transfer.

**52 KB of internal RAM recovered — and it root-causes TODO #65.** Found with three commands never previously run: `idf.py size` (`.bss` = 201 KB, 45 % of DIRAM), `idf.py size-components` (**186,986 B of it in `libmain.a`** — everything outside `main/` was noise), `nm --size-sort -S` (named the arrays). Moved to PSRAM: `s_worked` (22.5 KB ADIF worked-call cache), two 11.25 KB FT8 heard-table snapshots (now one shared buffer), `s_rows` (8 KB row-widget pool). Deliberately left internal: `audio.c`'s `raw[]`/`decoded[]` and the `dsp.c` FFT buffers — hot paths, and v0.19.4 proved a PSRAM spill makes the STFT ~10× slower. Measured: `.bss` 201,108 → 148,056 B; boot internal free 166 → 217 KB; steady 22–24 → 78 KB; **low-water mark 0 → 32 KB**. `MALLOC_CAP_DMA` went from ~311 B to **40 KB with WiFi fully up**, which corrects the long-standing claim that WiFi consumed that pool — our own `.bss` occupied the DMA-capable region and WiFi was merely the last straw. That makes ONE root cause of the SD remount failures, the USB endpoint-alloc failures on a QMX reconnect, the crypto failure above, and reboots under load.

**QMX power-cycle lock-up fixed.** Switching the radio off delivered no `AE_DISCONNECTED` to the audio side (a documented quirk — the UAC handle just goes quiet), so `audio_task` kept polling a dead handle. On a *silent* device the read honours its 25 ms timeout; on an **invalid-state** device it fails instantly, so the loop spun flat out at priority 6 → **core 0 at 0 % idle**, LVGL starved, UI frozen, nothing able to recover. The spin was starving the very task that delivers the disconnect event. `audio_task` now yields 5 ms when a poll moved no audio (zero effect while streaming). Verified: `usbh_devs_open` errors thousands → 1, the disconnect event arrives 20 ms later, `USB: All devices freed`, worst `idle0` 0.0 % → 7.2 %, and the radio **re-enumerates immediately**.

**WiFi remembers several networks (Roy KI0ER).** Up to 6, most-recently-used first, built implicitly from successful connections — nothing to maintain. When the configured network will not come up, it scans after two failed connects and switches to the strongest remembered network on the air; rate-limited to one attempt per 30 s so it keeps looking without hammering the radio. The typed-in SSID stays the *configured* one, so coming home is unchanged. Picking a known network from **Scan** fills its password in. Exported as `[wifi_known]`. Hardware-verified end to end, including the roam, by dropping a phone hotspot.

**Roy KI0ER's FT8 findings — four of five real.**
- **Finishing a QSO properly.** The re-send budget ran out after 3 attempts in 240 s and we then resumed CQ over a partner still repeating his report. Now 6 re-sends over 300 s, **and** once spent the Tab5 stays SILENT rather than calling CQ — a new hold flag checked in `rearm_current()`, the single choke point for CQ arming, with a disarm so a queued burst cannot fire anyway.
- **Occupancy strip filling up permanently** (his "turns totally red, cured by restarting"). The parity-aware row-aging *pause* had no upper bound, and during a CQ run we are parity-locked essentially always — so every row on our own transmit window was kept for the whole session, and `build_tone_occupancy()` reads that list filtered to our parity, i.e. exactly those rows. Capped at 10 minutes.
- **What the strip is showing.** It now carries an **EVEN / ODD / BOTH** tag; the tone picker states which window it describes.
- **Alternating the hunt window.** Every candidate in one robot tick shares a parity (only stations heard in *this* slot are eligible), so the bias is between QSOs: an exchange takes a fixed number of slots, so finishing one returns you to idle on the same parity. After 3 pounces on one window the robot yields one slot.

**Also:** the **QRZ / eQSL / LoTW setup is no longer hidden** behind having logged a QSO (Brian WA6JFK tried three browsers looking for it); the `SPOTS_MAX` cap was silently truncating the live feed; and a spot-store race that could draw a callsign at another station's frequency is fixed.

**Verification worth recording:** two boot self-tests (`spots_lane_selftest`, `rbn_selftest`) check the overlay geometry, age curve and row packing, and the RBN parser against **verbatim captured feed lines** — plus device spot counts cross-checked against a PC query of the live POTA API (`off L40 R34 / 0 visible` matched 40/0/34 exactly). RBN's parser vectors are real wire data, not invented alongside the parser.

**Still on-air-unverified:** the new final-resend conduct, the parity alternation, and Roy's item 2 (decodes drying up after an hour with the strip going fully green) — that one is a different fault from the stale-state bug and wants a field retest against the recovered memory before anything speculative is changed.

### Shipped in v1.5.0 — 2026-08-06

**The manual answers questions now, instead of being a manual.**

**Context-sensitive help.** The built-in manual can be opened *at the right place* rather than at its contents page. `main/ui/help_topics.c` holds one table mapping a topic to a page and a heading, so exactly one file knows which chapter covers what; `reader_view_open_help()` scrolls to the heading (case-insensitive substring, so renumbering and small rewordings do not break it). The drawer's **User Manual** button now lands where you are — the panadapter chapter, the FT8 *receive* chapter, or the FT8 *transmit* chapter when a burst is armed or active, because someone mid-transmission is asking a different question. Warnings you cannot act on became warnings you can: the IQ-mode banner and the "turn on your QMX" prompt are tappable. **Deep links cannot rot silently** — `tools/pack_manual.py` parses the table and fails the build if a page or heading has gone (and `mkdocs_reader_export.py` now *raises* on a packer failure, which it previously swallowed as a warning; a failed pack leaves the previous `manual.bin` in place, so that would have shipped a stale manual invisibly).

**"Need guidance?" — a short list of symptoms, ranked from what the device can see.** New drawer button beside User Manual, and the QMX-off prompt offers **"Need help?"** (an off radio is often off on purpose, so it does not imply a fault). The panel lists what you *see* — "Nothing appears in the decode list", not "no CAT link" — because a novice cannot map their symptom onto our vocabulary. Rows the firmware can tell are happening *now* are highlighted and float to the top: no CAT link, IQ never confirmed, an empty decode list, WiFi down when it is supposed to be up. **The device ranks, the operator chooses — it never navigates on inference**, however confident. The list scrolls, and every row is scoped to the screen it was opened from (nothing about the spectrum is offered in FT8, where none is drawn). Two supporting accessors added rather than worked around: `ft8_screen_active_count()`, so the decode count needs no ~11 KB snapshot buffer on taskLVGL, and `panadapter_wifi_is_enabled()`, so WiFi switched off for POTA is not reported as a fault.

**The manual is read from the firmware, not from a copy of itself.** Intermittent "Could not cache the page" reports were root-caused by an honest diagnostic rather than a guess: one `else` branch had been claiming "page not in the embedded manual" for *two* different failures, and the log showed the truth — `short write to /spiffs/reader.md: 0 of 12031 bytes`. The page was found; the write failed. `/spiffs` is 1 MB shared with `qso.adi`, the LoTW certificate and key, and the diagnostic log's 256 KB rolling file plus a rotation, so on a used device there is nothing left. Since v1.3.2 the manual is compiled *into* the firmware, so the cache was writing flash in order to read it back — the round-trip is gone, and pages and the contents list come straight from the blob. No free space required and nothing left to fail. That also retired the earlier "page not found" mystery: the same fault, wearing the wrong label.

**Tofu boxes gone, verified against the manual's actual contents.** An inventory of `docs/mkdocs/**` found **43 distinct non-ASCII codepoints**; 37 now fold explicitly and 6 are decorative emoji dropped on purpose. Two causes, neither the one guessed at first: `add_label()` set its text **raw**, so the heading and blockquote paths reached LVGL unfolded — folding now happens at the single point where text becomes a label, so no caller can bypass it. And the Block Elements range was not in the table at all: **U+2591 appears 128 times**, so the waterfall and occupancy sketches were not drawing boxes, they were silently *vanishing* and leaving captioned blank space. Also added: box-drawing diagonals as `/` and `\`, geometric shapes as `^ v > < o *`, `1/2`, `~`, and spelled-out `tau`/`alpha`/`ohm`. A last box survived that sweep and was found by the operator: both markdown walkers copied **link and image label text byte-for-byte**, bypassing the fold they applied to everything else — so an arrow in prose rendered fine while the same arrow inside `[...]` drew a box. All four copy sites now go through one helper.

**A help overlay owns the screen.** One operator screenshot produced five complaints and four distinct faults, all the same mistake in different places — a widget that puts itself on top and never stands down. The FT8 drawer stacked on itself because its reflow started sections at a hardcoded `y`, above a comment warning it had to be kept in step by hand (the next person to add a button did not); it now reads where the sections actually begin. The Reader's own **Back/Exit/Contents** could not be tapped at all: the top-bar Band/Mode/BW hit zones are direct children of the screen, foregrounded above the overlay, and LVGL hit-tests children in reverse creation order without considering siblings — so the BW zone won every touch aimed at them. Panadapter navigation stayed live over the manual: the edge-swipe strips, their breathing grips *and* the separate drawer-grip button are now hidden while a help overlay is up, re-asserted every second so they can never get stuck hidden. And the QMX-wait prompt, which re-foregrounds itself as a keepalive, now stands down for the drawer and the panel too.

**No more transition animations, and the reason generalises.** Landscape runs at ~13 fps because every flush goes through the software 90° rotation, so the Reader's 220 ms slide was about **three frames** — not motion, just two or three discrete snapshots at intermediate offsets, which read as a flicker going in and as an unrelated page surfacing coming out. A cross-fade would be the same three frames at 33/66/100 % opacity, so animating differently was not the answer: one correct frame in, one out. **Back** is also hidden when there is nothing to go back to, with **Exit** sliding into its place — two buttons where one is dead just makes the operator guess which one leaves.

**Call CQ from the browser (Dennis WN4FLA).** A CQ run that has timed out or reached its call limit needed a walk back to the Tab5. The button sits under the web page's TX status banner, confirms first (it keys the radio, and a mis-click from another room should not put a carrier on the air), and shares one code path with the Tab5's own Call CQ button so the two cannot drift over the TX-hold tone, the EVEN/ODD parity or the active preset. Deferred to the LVGL task via a flag — the QSO state machine belongs to that task — and the flag is consumed even when FT8 is not up, so a stale request cannot fire minutes later.

**The station you are working stays at the top of the decode list (Don WB0LQW).** During an exchange the partner's replies sorted down-screen where they had to be hunted for: "there is no station that I am as interested in as the one I am trying to contact." Part of it already worked — anything containing your own callsign sorted first — but not the partner's CQ while you are mid-exchange, nor a message to a third station (exactly when you most want to see it), nor a manual reply before anything has come back addressed to you. A new top tier handles all three, matched on the row's **callsign** rather than its text so a third station merely mentioning your partner is not promoted, and covering both a live engine-driven exchange and a hand-typed reply. It releases the moment the QSO completes, because their closing 73 contains your callsign and the existing tier already holds it up there. **Deliberately ranked above messages addressed to you:** mid-exchange they are the same row anyway, and where a third station calls at the same time, the contact in progress is still what you are looking at.

**Smaller things.** Live-spot off-screen counters read **"< spots (3)"** rather than a bare "<3" — a number against the edge of the spectrum says neither what it counts nor that it can be tapped. The raw `PC`/`SW` strings are now logged, not just the scaled watts, after a field report of the FT8 TX power reading (BD4AHS — resolved on his unit by 1.4.0, cause unexplained; the divisor has been wrong in each direction before, and a report like that cannot be settled without the bytes the radio actually sent).

**Two documentation corrections worth their own line, because both had produced field reports.** The FT8 robot's worked-before skip is **not** enforced — it follows the operator's *Exclude worked before* checkbox like every other filter, and the notes claiming otherwise are exactly why a user expected duplicates to be skipped without ticking it. And the QMX USB descriptor wedge is **an empty data stage, not a short descriptor**: the logged "Unexpected (8) … expected 16" counts ESP-IDF's own 8-byte setup packet in both figures, so the radio returned **zero** descriptor bytes for `GET_DESCRIPTOR(DEVICE, wLength=8)`. The earlier "8 of 16 bytes" phrasing — including in the report sent to Hans — would have sent someone hunting a truncated descriptor that does not exist.

**Still on-air-unverified:** **Don's decode-list pinning** — the mechanism is confirmed present and provably inert with no contact in progress, but the pin actually firing needs a real QSO; a change-detected log line reports it engaging and releasing so the first operator to make a contact can confirm it from the diagnostic log without watching the screen. Also unverified on the air: the web **Call CQ** button's final key-down (the endpoint, the thread hand-off, the preset/tone/parity reuse and the error path are all hardware-confirmed; "the radio transmits when pressed" is inferred from sharing the button's code), and the wording of the guidance panel's rows against how operators actually describe these faults.

### Shipped in v1.6.0 — 2026-08-09

**The browser became a second operating position, and the manual stopped being made of characters.**

**Full web/Tab5 parity.** The web page could show you the band; it could not work it. It now can. The **decode list** arrives ordered by the device rather than re-sorted in the browser, so the two screens cannot disagree about who is at the top. **Replying to a station is the same tap** it is on the Tab5 — the same intelligent-Transmit path that decides which message comes next, with a confirmation step and the outcome reported back, because a mis-click from another room should not put a carrier on the air. Added with it: **live spots on the browser spectrum**, the **TX tone picker** over a live occupancy strip, **memory channels**, **settings you can type** (callsign, grid, the three CQ messages, the FT8 filters), the **pileup and grey-list views**, the **robot toggle carrying the same permanent unattended-TX warning** it has on the Tab5, **Antenna Tune** with its 60-second safety, the **time-source label**, and **help plus the entire manual served by the Tab5 itself** — no internet, and always the manual that matches the running firmware.

**`qmx.local`.** The WiFi layer picks its own network from up to six remembered, so the IP changes without anyone deciding it should — and the only place it was shown is the Tab5's own bottom bar, which is useless when the Tab5 is in the shack and you are not. An mDNS responder now answers to `qmx.local`. It cost internal RAM until the cause was measured rather than assumed: the component sizes its **static tables** for three interfaces and ten services, and we have exactly one of each — cutting those took the internal low-water mark from ~9 KB back to 22 KB. The PSRAM task settings tried first made **no measurable difference**; they are kept only because they match this project's standing rule for background tasks.

**Do not zero-beat the DX (Roy KI0ER).** Everyone answering a CW CQ zero-beat arrives as one mud-pit, and a QRP station 400–600 Hz off stands out. The QMX has no XIT — its own CAT manual says so — so this is **split**: receive on VFO A, transmit on VFO B held at A plus your offset. It is maintained in the CAT poll task rather than hooked onto our own tune path, which is what makes it follow **every** way the frequency can move: a tap on the panadapter, a spot click, a memory recall, a band change, the web page, and the radio's own tuning knob. CW and CW-R only; leaving CW clears it; it only ever clears split if the Tab5 set it, so an operator running their own split is left alone; and it re-asserts every 30 seconds so a radio that dropped split cannot quietly transmit on top of the DX. `FB`/`FR`/`FT`/`SP` are all in the 1.03 manual, so this needs no firmware gate. **Not verified on the air** — the CAT round-trip is confirmed on the bench, "does the DX hear me 500 Hz up" is not.

**RF gain, and a way to hand the radio back (Stan KC7XE, via Samuel W7STF).** RF gain is now a slider under the QMX volume, in dB, 0–99. Two things it does differently from the volume, both deliberate: it is a **per-band** value — the same figure the Band Configuration screen edits — so the Tab5 reads it from the radio instead of remembering a number that would belong to whichever band you were on last; and it writes on release rather than continuously, because unlike volume this is stored configuration. **Release radio** stops all CAT traffic so the QMX's own menu and its Terminal Applications have the port to themselves. That turned out to be more than a convenience: it also stands down the watchdogs, which read a deliberate menu visit as a fault and would eventually have cut USB power under the operator's hands.

**A radio that stopped sending audio now recovers itself (Roy KI0ER).** Decode lists went blank while transmit still worked, and only a QMX restart fixed it. Chasing it produced the mechanism, with Stan's analysis in a separate thread supplying the missing half: a trip through the QMX's front-panel menu can trip its own watchdog reset, the radio restarts, and **`Q9` IQ mode is session state**, so it is switched off — while the USB link survives the restart without the Tab5 seeing any disconnect. The result is a radio that answers every CAT command perfectly and sends no audio. The watchdog now escalates cheapest-first: at 30 s it simply **asks for IQ mode again** (free, invisible, and it fixes that case outright), at 60 s a soft audio reset, at 120 s the USB power-cycle you would otherwise perform by hand — capped so a genuinely dead radio cannot loop it.

**FT8 transmit-window fixes (Roy KI0ER).** Every occupancy strip — the mini strip on the FT8 pane, the full picker, and the web page's — now shows **both time windows, EVEN above ODD, always**. Your marker sits on your own window once a transmission has fixed it, and on both before that, because until then the tone is chosen but the window is not. The **"FREQ BUSY" warning never received the parity filter** the occupancy map got in v1.3.4, so it was counting stations in the window that cannot collide with you — which is exactly why it contradicted the green strip. The **auto-answer robot** now abandons a pick that turns out to be mid-QSO with somebody else and chooses another caller, rather than waiting through a contact it was never part of; a deliberate pounce still holds, because a willing pounce means you want that station. And a **listening slot** can be spent every N calls during a CQ run, so the picture of your own window stays current — off by default, since it changes on-air cadence.

**The manual's drawings are drawn now.** They were built from UTF-8 box characters, and the Reader folds UTF-8 to ASCII with **some folds changing the length of the line** — an arrow becomes `(right)`, one character into seven — so a line the author had aligned overran its box and wrapped. No font fixes that (a monospace font was tried and reverted the same day). Instead a fenced `qmxdiagram` block carries a **semantic** spec — what the parts are, never where they go — and it is rendered twice: by the Tab5 with its own widgets and colours, and to SVG at build time for the website, so the two surfaces cannot drift. All **12 drawings** converted, four diagram types, every spec validated by the packer at build time.

Redrawing them was an audit, and **seven statements turned out to be untrue**. The FT8 slot timeline claimed the receive window both ran to 14.999 s and closed at 4.5 s, and put the transmit opportunity at 4.0–4.5 s; the code cuts capture at **13.2 s** and puts the reply window in the **first 2.8 s** — the opposite end of the slot. The candidate cap is 140, not "~10". The CQ preset drawing still showed radio buttons for a dialog that has checkboxes. And the status bar has never had the **FPS readout** three separate documents claimed — the only frame rate in the tree is the WebSocket stream rate, which is why it appears on the web page and nowhere on the device.

**An A–Z index (context help, Layer 4).** 256 terms, one per heading, generated into the manual and sorted on the first real word — "6. Tap to Reply" files under T, where you would look for it. Deliberately data rather than a page of links: the Reader renders links as plain text, so an index written that way would have looked right and done nothing. Letters first, then that letter's terms, because 256 terms is 256 widgets and at ~13 fps building them all at once is a visible stall. The guidance panel's bottom button finally says **"Look it up (A–Z)"** — it read "Open the manual" precisely because this did not exist.

**The drawer is grouped.** Twenty-five sections had accumulated in build order, so the two QMX gain controls sat together but CW pitch was nine sections from the CW transmit offset. They are now **Station / Device / Radio / Network / Display / FT8 / Spectrum** under headings, with a **Basic/Expert** toggle beside the Settings title that says both where you are and what a tap gives you. Sections are positioned by a layout pass rather than rebuilt, and heights come from the sections themselves — the parallel table that had to be kept in step by hand had already fallen out of step twice.

**Web fixes found by using it.** Every manual page in a subdirectory returned **404 in the browser** — 15 of 19 — because the handler never URL-decoded its query and the browser sends `guide%2Fpanadapter.md`. Spots were not failing to stack, they were being **deleted**: all labels shared one row and any that would overlap was dropped along with its line, so a busy patch showed fewer spots than the counter claimed. The memory page could not reach its own bottom rows, and its instruction to "click a channel to tune to it" was never wired to anything but the buttons; channels can now be dragged to another slot or onto a bin. Save/Cancel/Close carry the Tab5's colour language instead of all being the same grey pill.

**Spots settle instead of jumping.** The store replaced its whole POTA slice every 60 seconds, so a station the API happened to omit from one response vanished and returned a minute later — and the order changed wholesale, which re-shuffled which spots got a label at all. It now merges: a fetch refreshes what it mentions and leaves the rest to age out. The Tab5 and the browser were also reading **different pools** — the browser scoped to the current band, the Tab5 taking every spot on every band — which is why their off-screen counts never agreed.

**Still unverified on the air:** the CW transmit offset (bench-verified over CAT, never keyed), the CQ listening slot's cadence, and the dead-stream watchdog's 30-second IQ re-assert against a real field occurrence.

---

### Shipped in v1.7.0 — 2026-08-09

**A Bluetooth mouse, phone spots, and knowing who can hear you.**

- **Bluetooth mouse.** A pointer that works *while the QMX is connected* — the
  case a USB mouse can never serve, because the radio owns the Tab5's only USB
  host and sharing it through a hub does not work on this hardware. Pair once;
  it reconnects by itself from then on, across reboots and firmware updates.
  Move, left click, and a scroll wheel that scrolls whatever is under the
  pointer. A Bluetooth symbol in the bottom bar shows off / scanning /
  connected, mirrored in the web UI. Off by default; enabling it needs a
  restart, because Bluetooth can only start once the link to the wireless
  co-processor is up. Costs about 5 KB of memory — the pools live in external
  RAM and the stack is sized for exactly one mouse, which is what keeps it
  affordable on this board.
- **DX cluster spots — the phone spots that were missing.** RBN is automated
  skimmers, and no machine can recognise a callsign spoken into a microphone,
  so every SSB station on the band was invisible. A cluster is people typing.
  Third source into the same lane, with mode worked out from the spotter's
  comment or your band plan, park/summit references picked out of the text, and
  relayed skimmer spots dropped so they cannot double what RBN already shows.
  Off by default.
- **Activation logging (POTA / SOTA).** Start an activation and every logged
  contact carries the reference automatically — with the contact count shown
  against the threshold, read from the log itself. Chases are tagged too, from
  spots the panadapter has already seen. The ADIF download can be limited to a
  single reference, so you upload that park's log rather than your whole file.
  Stopping is as prominent as starting, because the failure that actually
  happens is driving home with it still on.
- **SWR protection while transmitting.** The QMX reports SWR over the control
  link; above your limit (3.0:1 by default) the transmission is cut short and
  the transmitter latched off until you clear it. An FT8 burst is thirteen
  seconds of key-down, so a disconnected or wrong-band antenna has real time to
  do damage.
- **Propagation feedback — "who is hearing me".** Asks PSK Reporter which
  receivers have copied *your* call, listing distance, bearing and the report
  they gave you. The valuable case is the mismatch: stations you can hear that
  cannot hear you is a transmit-side fault, and from the receiving side alone it
  looks exactly like a dead band.
- **Spot lane de-duplication.** An activator spotted on POTA *and* heard by RBN
  was drawn twice, in two colours, at almost the same place; RBN also doubled
  itself where two skimmers rounded the same signal differently. One station is
  now one entry, and the RBN sighting is folded in as corroboration — a
  brighter marker meaning a receiver actually copied them just now, rather than
  a self-spot typed an hour ago.
- **Clock said GPS with no radio attached** (Don N2VGU). Once a QMX+ has been
  seen to be GPS-disciplined that is remembered, so an offline start keeps GPS
  timing without re-detecting it — but the clock label trusted that memory
  without checking the radio was still there, and kept claiming GPS while NTP
  was actually keeping the time. The Tab5 has no GPS of its own. The time was
  always correct; only the label was wrong.
- **Fixes.** A tune started from the web UI now says so on the Tab5 instead of
  transmitting silently. Antenna Tune no longer cancels itself after a second
  and leave the radio keyed — it was reading a mode value the QMX deliberately
  does not report while tuning. Manual corrections: the panadapter page
  documented an FPS readout and a colour-map label that have never existed.

### Shipped in v1.7.1 — 2026-08-10

**Tried a Bluetooth mouse on v1.7.0? Install this patch so your WiFi doesn't go flaky.**

- **The Bluetooth mouse was making WiFi unstable, and nothing said so.** Turning
  the mouse on could leave the web UI flapping between Connected and
  Disconnected, with the connection dropping and recovering for as long as it
  stayed on. The two look completely unrelated, which is why it took days to
  find: WiFi and Bluetooth share one link to the wireless co-processor, and the
  Tab5 was listening for *every* Bluetooth device in the building, continuously,
  forever. In a busy room that is thousands of advertisements a minute, and WiFi
  was left fighting for the same pipe. It now listens in short bursts, and once
  your mouse is paired the co-processor is told to ignore everything else — so
  the traffic never reaches the Tab5 at all. Measured on the bench: the
  underlying link errors went from about five a minute to **none**, with the
  mouse still connecting by itself and working normally. If you have not used a
  Bluetooth mouse, nothing changes for you.
- **The settings drawer would not close.** Swiping it away worked only if your
  finger happened to start on bare background — anywhere on a slider, a checkbox
  or a label and nothing happened, which felt random because it was. **Tap
  anywhere outside the drawer to close it.** (Michael KZ4LY)
- **CW transmit offset is settable by the hertz.** The slider covers 2000 Hz in
  10 Hz steps, which is about two pixels a step — fine for a rough setting,
  hopeless for an exact one. There are now **−50 / −10 / +10 / +50** buttons
  under it. The range is unchanged, deliberately: it runs to ±1 kHz because a
  station calling CQ wants to stand out, while someone breaking a pileup rarely
  moves more than 100 Hz. (Michael KZ4LY)
- **DX cluster spots stopped disappearing.** A cluster that simply had nobody
  typing was treated as a dead connection and dropped, roughly every 70 seconds
  — losing the spots it was holding each time. Quiet is normal on a cluster; it
  now stays connected.
- **The browser said GPS with no radio attached.** The same fault Don N2VGU
  reported in the clock, fixed on the Tab5 in v1.7.0 but missed in the web page.
- **A failed WiFi scan now says why.** "Scan failed — tap Scan to try again"
  used to leave nothing in the diagnostic log to explain it, and a scan that
  never came back would sit on "Scanning..." indefinitely.


### Shipped in v1.7.2 — 2026-08-10

**A Bluetooth mouse could stop reconnecting until you restarted the Tab5.**

- If a reconnection attempt failed part-way — which happens routinely, because
  the mouse powers itself down after about half a minute to save its battery —
  the Tab5 stopped trying. The pointer never came back, even with the mouse
  awake and right beside it, and only a restart fixed it. Verified over 14
  sleep/wake cycles: fourteen disconnects, fourteen reconnects, none missed.

Nothing else changed. If you do not use a Bluetooth mouse there is no reason to
install this.


### Shipped in v1.8.0 — 2026-08-11

- **SOTA spots.** Summit activations now appear on the spectrum alongside POTA, RBN
  and the DX cluster, fetched from [spothole.app](https://spothole.app) — Ian Renton
  M0TRT's aggregator, used with his permission. **Off by default**: it is a
  volunteer-run server, so nobody polls it who has not asked. Fetched once every two
  minutes, and when it is unreachable the spots already on screen simply stay until
  they age out — no banner, no error, nothing to dismiss.
  - Two silent bugs fell out of it, both older than this feature: a 12-character
    portable callsign (`HB0/HB9BXQ/P`) lost its last character, and a 10-character
    summit reference (`EA1/AT-125`) was logged as `EA1/AT-12` — a *different*
    summit. DX-cluster spots were affected too.
  - A chase is now logged with the right award programme: a summit worked via a
    cluster spot no longer goes into the log as POTA.
- **Fox/Hound (DXpedition) mode — hound side.** See the FT8 Transmit chapter. Off /
  Guided / Automatic. The Tab5 calls from above 1000 Hz, moves onto the Fox's own
  frequency when answered, and stops on its `RR73` without sending a `73`.
  Recognises a Fox by watching it work a queue rather than by frequency alone, and
  the transmit window says "as HOUND — will QSY onto NNN Hz" before you commit.
  **Validated in simulation only — no real DXpedition has seen it yet.** The Tab5
  cannot act as a Fox: that needs five simultaneous signals and the QMX is keyed one
  tone at a time.
- **Mouse and pointer.** A proper arrow instead of a dot, which turns **bright
  green** over anything a click would act on — a touch UI has no hover states, so
  what is live was previously invisible to a mouse user. The edge grips accept a
  click (a pointer cannot swipe), and the settings drawer and Memory Channels grew
  handles you can simply tap. The scroll wheel no longer jams when the pointer
  passes over a dropdown.
- **Spot mode filter.** "Mode filter the spots" hides spots you cannot work in the
  mode you are in — tapping a spot sets the mode as well as the frequency, so an
  unfiltered lane could drop a CW operator into FT8.
- **CW transmit offset narrowed to ±300 Hz**, and a stored wider value from v1.7.1
  is clamped on the next start. **The guidance was wrong too, and is corrected
  everywhere**: v1.6.0 and v1.7.x suggested 400–600 Hz, which is outside many
  operators' filters altogether. Around 100 Hz or less is what actually works — the
  aim is to land *inside* the other station's passband, most CW operators run 500 Hz
  or narrower (200 Hz is not unusual), and a QRP signal has least energy exactly
  where the filter is already attenuating (Michael KZ4LY). Roy KI0ER, who asked for
  the feature, uses +60 Hz.
- **Fixes.** The flash-persisted diagnostic log could stop writing silently and stay
  stopped (it now reports the failure and recovers by rotating); the "power-cycle the
  QMX" pop-up is gone, since it fired at a radio that was merely switched off and the
  screen already says so; the "Now turn on or reboot your QMX" prompt now stands down
  whenever any window is open instead of drawing across it.

- **RIT — receive off your transmit frequency, by tapping.** Asked for by Roy KI0ER,
  and shaped with Michael KZ4LY and Bill Carver: while you are running a frequency, a
  caller answering slightly off it can be pulled in without moving transmit. A **RIT**
  button at the top right of the spectrum arms it; a tap on the spectrum or waterfall
  then sets the receive offset onto whatever you tapped instead of retuning, and it
  stays armed so the next caller is another tap. A dashed magenta marker shows where
  you are listening, in the spectrum and down the waterfall, while the gold line goes
  on meaning the dial — and therefore transmit. The filter window moves onto the caller
  too. Retuning clears it, from every path. Available from the browser as well.
  - The QMX has real RIT in both 1_03 and 1_04, so unlike the CW transmit offset this
    needs no split trickery. ⚠ `RU`/`RD` are absolute *or* relative depending on a QMX
    menu setting the Tab5 cannot read, so every write clears to zero first and then
    moves once — which lands correctly under either setting.
  - Tap-to-RIT uses a 10 Hz grid whatever the mode. The normal tuning grid exists to
    land the *dial* on a tidy frequency; SSB's 250 Hz would have left five usable
    offsets inside the ±500 Hz limit.
  - ⚠ **The display sign is derived, not verified on a signal** — there is no antenna
    on the bench. If a steady carrier jumps when you engage RIT instead of standing
    still, say so and it is a one-character fix.

- **The browser caught up with the Tab5.** A control-by-control comparison, 87 commits
  after the last one. RIT and its marker; starting and stopping a POTA/SOTA activation,
  with a badge that appears while one is running so it cannot be forgotten; CW pitch,
  IF calibration, the battery charge limit, the 180° screen flip, Fox/Hound, simulation
  mode and the spot mode filter; and a "Prepare for flashing" item for an action that
  had existed since v1.6.0 with nothing to call it. The only thing still Tab5-only is
  the clock-sync window.

- **SWR protection could not be saved from the browser — check yours.** The field was
  shown, accepted your value, reported success, and the device discarded it. It has
  been that way on every v1.7.x build, so anyone who set it there and did not check the
  Tab5 has been transmitting without it. The browser now offers the same four
  thresholds the Tab5 does rather than asking for a number.

- **The browser's spectrum and waterfall now look like the Tab5's.** Samuel W7STF asked
  whether the black-level and contrast settings reached the browser. They did not, and
  chasing that found four separate faults:
  - the spectrum was quantised for transmission with its floor at **−130 dBm**, which is
    exactly where this receiver's own noise sits, so a quarter of the band arrived
    flattened against the bottom of the scale and the browser had nothing to measure a
    noise floor against. Widened to −150 dBm;
  - the browser was not applying the smoothing stage the Tab5 applies before drawing, so
    the **Spectrum smoothing** setting did nothing there;
  - it used its own colour ramp and its own floor arithmetic instead of the Tab5's;
  - and the Tab5 **re-seeds its noise floor about seventeen times a second**, so the
    per-bin adaptive floor has never actually run on any version. What the Tab5 draws is
    each bin measured against the average across the band, refreshed constantly — which
    is why it settles instantly and never lets a signal fade. The browser now does the
    same. **Consequence: the "Adaptive floor" setting does nothing and never has**, so it
    has been removed from the browser's settings page. The behaviour is deliberately left
    alone — the alternative is what it was designed for, where a steady carrier sinks out
    of sight over about a minute.

- **Spot labels stopped stealing clicks.** Samuel W7STF's "tuning oddity": clicking a
  signal in the browser put the VFO on a plausible nearby frequency where nothing could
  be heard. Clicking a callsign takes you to that station, which is intended — but the
  label's clickable area was as wide as the callsign and sat at the same height as the
  trace, so `OK/DL4ROB/P` owned **3.8 kHz** of band in which every click went to that
  station. The label no longer claims space above and below its text, so a click
  elsewhere in the same column tunes where you clicked, and hovering a label now shows a
  marker at the station's real frequency with its callsign, so nothing happens silently.
  Two more found in the same code: in CW every spot was drawn **700 Hz to the left** of
  its own signal, and the browser rounded clicks to a different grid than the Tab5
  (500 Hz in SSB where the Tab5 uses 250).

- **Bluetooth mice that are not the one on the bench.** The Tab5 assumed a single byte
  layout for movement — the 12-bit one a Logitech here sends. A mouse using 16-bit
  movement decoded to a vertical value about **sixteen times too large**, which drove the
  cursor into the top edge and held it there while horizontal still worked: exactly what
  Samuel W7STF reported. It now reads the descriptor every HID mouse publishes and
  decodes accordingly, with the old fixed layouts kept as a fallback. Tested against
  three real descriptors on the host; **not yet confirmed against his mouse**, and the
  log now records both the descriptor and what was made of it.

- **Two browsers no longer fight over the live view.** The Tab5 streams the spectrum to
  one browser at a time and the newest connection wins — but the displaced one used to
  retry after two seconds and take it back, so a second tab, a phone or a laptop left
  open made both flap between "reconnecting" forever. One session here logged **340**
  handovers. The displaced browser is now told, stops retrying, and says "another browser
  took the live view — click to take it back".

- **"Release radio" reworded** (Samuel W7STF: "not too helpful, in fact confusing"). It
  described what the software does to the radio; it now says what you are trying to do —
  **"Let me use the QMX menus"**, and **"Done - Tab5 takes over again"** while it is
  handed over. The pause and play icons are gone; a gear points at where you are headed.

### Shipped in v1.8.1 — 2026-08-12

A fixes release, almost all of it from field reports the day v1.8.0 landed.

- **The spectrum is tunable again wherever you can see it.** Most of the spectrum had
  stopped responding to tap-to-tune: only a window in the middle worked, so it felt like
  tuning only worked near the centre frequency, and the waterfall was fine. A change the
  week before had made the top-bar Band/Mode/BW/Zoom touch areas shallower so they would
  stop swallowing taps on the spot callsigns — but the tune code still assumed the old,
  deeper areas, leaving a band of the spectrum that belonged to nobody. Nothing opened and
  nothing tuned. The rule now is simply that **if the mouse pointer is white, clicking
  tunes**, and a finger behaves identically. Found with a mouse; a fingertip lands lower
  and rarely met it.

- **CW centre could not reach the value your radio uses** (Samuel W7STF, Roy KI0ER). It is
  the sidetone: with the QMX default, the filter centre moves the CW offset and sidetone
  with it. The radio offers **500 to 950 Hz in 25 Hz steps** — which of them you can pick
  depends on the CW filter width you have chosen — while the slider ran 600 to 800 in
  50 Hz steps. Wrong at both ends and on the wrong grid.
  - It also could not agree with the radio. The Tab5 pushed its stored value about four
    seconds into boot, and CAT does not exist until about seventeen, so that write never
    arrived on any boot. The Tab5 now **reads the centre from the radio** when the link
    comes up, which is what Roy asked for.

- **The CW transmit offset now clears up after itself** (Roy KI0ER). Switching it off did
  return the radio to simplex, so FT8 was never transmitting off frequency — but VFO B was
  abandoned at A plus the offset for the rest of the session, which is what the radio was
  showing. VFO B is now written back to match VFO A *before* the split is dropped (the QMX
  will not take it afterwards), and simplex is confirmed by reading it back.
  - **Still outstanding, and it is on the radio's side:** the QMX goes on displaying both
    VFOs even though it reports VFO A mode and simplex. Only a configuration reload or a
    power cycle clears it, and the reload also switches off the radio's IQ mode, which
    would cost you the spectrum — too high a price for a display. Reported to QRP Labs.
    Roy's workaround: switch band and back.

- **RF gain and volume disagreed between the Tab5 and the web page** (Samuel W7STF). Both
  read a cached value that only updated when the radio answered a query, so whichever
  screen you opened second showed the number from before your change.

- **Spot labels no longer claim kilohertz they do not own** (Samuel W7STF). In the browser
  each label was a click target across its whole width — a callsign can be 3 to 4 kHz wide
  on screen — with the spot's real frequency somewhere in the middle, so clicking a signal
  a couple of kHz from a spotted station took you to the station instead. A label is now a
  target only close to the frequency it marks.

- **The seconds can be set again** (Don WB0LQW, gesture by Roy KI0ER). Hours and minutes
  were editable and the seconds were not, so with no WiFi and no GPS there was no way to
  get the clock inside the second FT8 needs — which is exactly where Don was on a POTA
  activation. **Hold the SS box and release on the minute.** The clock is set on release,
  not when you tap Save, because the release is the measurement.

- **A "Show RIT button" setting** (Samuel W7STF), in the drawer under Radio and in the web
  settings, on by default. Turn it off if you never use RIT and would rather have the
  corner. If RIT is actually engaged the button appears anyway — a radio listening off
  frequency with nothing on screen saying so is not a tidy screen.

- **Saving settings from the web page could fail with "HTTP 400"**. The handler read the
  form into a fixed 1 KB buffer with a single read and no loop, so it worked only while the
  form stayed under that size. One more checkbox took it over and the whole save failed
  with nothing to say the body had been cut short.

- **The manual now states that a Bluetooth mouse must be BLE 4.0 or later.** The Tab5's
  Bluetooth comes from a co-processor with no Bluetooth Classic radio, so an older Classic
  mouse can never work and no firmware change can help — it never appears at all. Roy
  KI0ER's two Microsoft mice are that case; his MX Master is Low Energy and works.

- **Bluetooth mouse decoding is unchanged in this release.** The real fault was found — the
  Tab5 was reading only the first 22 bytes of the layout description every mouse publishes,
  so the description never made sense and it fell back to guessing — but the rewrite broke
  the mouse another way and was reverted. It will be redone properly. Samuel W7STF's
  symptom does not match the fault that was found, so there is likely a second one.

- **The QMX USB wedge (#74) was investigated properly and the diagnosis stands.** Six
  approaches were tried against a genuinely wedged radio, covering both recovery and
  prevention, including an orderly shutdown and holding USB power off through an entire
  reboot. All produced the identical failure: the radio acknowledges the setup packet and
  returns **zero** descriptor bytes. Nothing the Tab5 does changes it and only a QMX power
  cycle clears it. The sdkconfig records the negative results so nobody raises those
  timings again hoping.

## v1.8.2 — 2026-08-13

A field-report release. Everything here came from an operator's message this week.

### The radio's own spurs can now be removed from the display (new, off by default)

At some dial frequencies the QMX puts a comb of evenly spaced artifacts into the IQ
stream. They hold their position whatever you do, they do not move when you tune, and
they are still there with the antenna disconnected. Measured on the bench at 14.074 —
the FT8 calling frequency — the strongest sat **38.6 dB above the noise floor**, with a
second harmonic and a mirror image, and the noise floor itself was 5–6 dB worse there
than 6 kHz away.

A frequency sweep found the law behind them: a spur's baseband offset moves at
**16–50× the dial** (16.0 and 23.0 measured over several intervals). That is what makes
them findable without any calibration. Nudge the dial 25 Hz and a spur jumps 8–27 FFT
bins while a real signal moves half of one — a physical discriminator with an order of
magnitude of margin, not a statistical guess. Three measurements at *f*, *f*+25 and *f*
again identify bins that are strong and steady at *f* but collapse when the local
oscillator moves. Baseband DC is handled separately, because bin 0 cannot move when the
oscillator does.

**Settings → Waterfall → Spur suppression**, three positions:

- **Off** (default) — nothing is touched, and the marker code does not draw a pixel.
- **Subtract spur power** — removes the measured artifact in the power domain, so a real
  signal sharing a bin keeps its own contribution. Limited to about 12–17 dB: the spur's
  own level wobbles a few tenths of a dB, and a constant cannot cancel something that
  moves.
- **Erase spur bins** — interpolates the affected bins away. The comb disappears (+38.6
  dB → about +10 dB, below the waterfall's black level). A real signal sitting exactly on
  one is hidden while the dial sits still; nudging the dial slides the blind spot off it.

Results are cached per frequency, so returning to a frequency already learned needs no
nudge at all — which matters because FT8 sits on one frequency for hours. Detection takes
about 1.9 s and only fires 600 ms after the dial stops, so it never runs while tuning.
Suppressed bins are marked in teal on the line under the frequency labels, so what the
firmware is touching is always visible.

### A QMX without GPS no longer overwrites an accurate clock (Don WB0LQW)

Set the Tab5's RTC accurately at home, arrive at a POTA site, switch the radio on — and
the accurate UTC was replaced by the radio's power-on 00:00, after which FT8 stopped
decoding.

The clock was only ever protected from the radio while SNTP was fresh, and **offline SNTP
is never fresh**, so the protection could not help the one case that needed it. A QMX
without GPS is not a time reference: its RTC free-runs and restarts at 00:00 after any
power-off, while the Tab5's supercap RTC holds seconds-accurate UTC for 30–40 hours. The
radio is now refused whenever the Tab5 holds a clock it trusts, and the Tab5 sets the
radio instead — only when the radio is more than 3 seconds out. If the Tab5 has no good
time either, a QMX reading is still used, because something beats nothing.

### RIT can be parked and restored (Roy KI0ER)

**Long-press the RIT button** and the offset is remembered while RIT switches off;
long-press again and it comes back unchanged. Short press still clears it outright. The
button reads `RIT (+250)` in brackets while an offset is parked, so there is always
something on screen saying an offset is waiting — including when the button itself has
been switched off in settings. A parked offset is discarded when you retune, because it
belonged to the station that was off frequency.

This is for a round robin or a net where one station is off frequency: the offset comes
and goes as the turn passes, without re-dialling it each time.

### The RIT offset is shown on the waterfall (Samuel W7STF)

The offset now prints beside its own marker, as `+250 Hz`, so it can be read off the
spectrum rather than from the corner.

### The band strip stays visible out of band (Samuel W7STF)

It used to hide its contents entirely and leave an empty row, which reads as a fault. It
now fills with one flat block reading **"Out of band"** and returns to the CW/Digi/Phone
colours as soon as you are back inside a band. The marker and visible-span block stay
hidden while out, because there is no band plan to position them against. The strip
staying put also keeps the coarse-tune drag where the thumb expects it.

### The Operator Identity window no longer appears for unrelated faults (Don WB0LQW)

Calling CQ with a message that would not build popped the Operator Identity editor
whatever the actual cause, so an operator whose callsign and grid were perfectly fine was
sent to check them. The real error is now reported, and the identity editor is only
offered when the error is genuinely about identity.

Investigating that report also produced `test/ft8_cq_encode_harness.c`, which runs CQ
presets through the real encoder on a PC. It cleared the encoder of the underlying
report: `CQ POTA <call>` and `CQ QRP <call>` encode and round-trip correctly with and
without a grid. That fault is still open and needs a diagnostic log from the field.

### The panadapter no longer power-cycles a wedged radio for nothing

After certain Tab5 restarts the QMX answers every USB enumeration with an empty data
stage, and only a QMX power cycle clears it. The recovery that watches for a stuck USB
port was firing at this, cutting the port's 5 V for two seconds — which switches the
radio off in front of the operator and, as six falsified approaches on the bench already
showed, achieves nothing against this particular fault. It now recognises the radio-side
signature and leaves the radio alone, logging what is actually wrong. A genuinely stuck
port still gets its recovery, which is the case where it works.

### Documentation

- The offline/POTA section now covers switching the radio on, which is where Don lost his
  clock.
- A second frequency left on the QMX's own LCD after the CW transmit offset stands down
  is documented as a display artifact of the radio, confirmed by Stan KC7XE and known on
  the QRP Labs list. VFO B equals VFO A and split is off, both verified by read-back.
  `MU;` clears the display but silently drops I/Q mode, and the manual now says so.
- **"Adaptive floor" is documented as having no effect.** The per-bin floor it blends
  towards is re-seeded many times a second, so both ends of the slider produce the same
  picture. It was already absent from the browser's settings form for that reason; now
  the manual says it plainly rather than describing a control that does nothing.

## v1.8.3 — 2026-08-14

**A field-report release. Every fix in it was found by someone using the radio, and
most of them by one person.**

### QRZ and eQSL credentials can be re-entered (Brian WA6JFK)

The prompt for an API key or login only ever appeared when **nothing** was stored, so
a key typed wrongly — or one the service later reissued — could not be replaced from
the web page at all. The remaining routes were editing the config file by hand or a
full erase-and-reflash, which is what Brian asked whether he needed. He did not.

There are now visible **Change QRZ API key** and **Change eQSL login** rows under the
upload links, which appear once something is stored. LoTW already had Ctrl-click for
this, but a hidden gesture was not the answer either — Brian is the same operator who
could not find the QRZ setup at all in August, which is why that menu stopped hiding
itself.

**Works today on any build:** Files → Config download, edit the `qrz_key`, `eqsl_user`
and `eqsl_pass` lines, Files → Config upload. The new details are used by the very next
upload with no restart. That file holds your WiFi password in plain text too.

### The dB scale labels follow the range you set (Samuel W7STF)

They were hardcoded `-40 / -60 / -80 / -100 / -120`, which silently assumed the default
−130..−30 range. Set Min/Max to anything else — Samuel ran −118/−13 — and the labels
described a scale that was no longer there.

Now derived: the firmware picks the smallest round step that still fits in five labels,
so a **narrow range gets finer lines rather than fewer**. At the default range the
result is byte-for-byte the old five values, so this is invisible to anyone who never
touched the sliders.

The arithmetic lives in `util/db_gridlines.c`, deliberately portable so
`test/db_gridlines_harness.c` can link the real function rather than a copy. It earned
that immediately: the harness caught a wrong expectation, and a mutation run found that
no test reached the case where the `n > max_n` clamp is the only thing preventing a
write past the caller's five-element array.

### The Adaptive floor slider is gone (Samuel W7STF)

He asked why the waterfall has so many handles. The answer was that one of them did
nothing at all: the per-bin noise floor it blends towards is re-seeded many times a
second, so both ends of the slider produce the same picture. It had already been
removed from the browser for that reason. A control that cannot change anything is
worse than a missing one.

The stored value is kept, still exported with your configuration and still accepted by
the API, so the row returns the day the underlying floor tracking runs.

### The shaded passband is drawn where the radio actually filters (Samuel W7STF)

He measured a gap of about 250 Hz between the dial frequency and the start of the
shaded region. That edge was a fixed 200 Hz that never came from the radio.

Two errors, and the second is the one worth remembering. Digital modes do **not** use
the selectable SSB filters — the QMX operation manual names one fixed *"150-3200Hz wide
filter used for Digital modes"*. And in DiGi the radio reports `FW;` as **3200**, which
is the filter's **top edge, not a width**: 150 + 3200 would be 3350 and contradict the
manual. So the shading was drawn at 200–2900 where the radio is 150–3200.

Corrected and then measured on the screen: with a 37.5 Hz/px axis and the dial cursor
at x=641, the tint runs x=645–727, i.e. +150 Hz to about +3225 Hz.

The low corner for the **SSB** filters is not documented anywhere findable, so that one
is deliberately left at 200 and marked unverified rather than made tidy on an inference.

### RF gain no longer sticks on "reading…" (Samuel W7STF)

It asked the radio for the value and then painted the answer to the **previous** ask, so
on the first drawer open after boot there was nothing to paint — and nothing repainted
it when the reply landed. It cleared only on the *next* open, and a single unanswered
query left it stuck for the whole session. Since the gain is stored per band, it
returned on every band change.

It now fills itself in as soon as the radio replies, and says **"radio not connected"**
when the radio is not there, instead of a "reading…" that implied a conversation was in
progress.

*(The first version of this fix was falsified on hardware and rewritten: it counted its
10 s budget from drawer open, but CAT link-up is ~17 s after boot, so opening settings
early — the normal thing to do — expired the wait before the radio could possibly
answer. The budget now runs only while the CAT link is up.)*

### The dark bands at the edges of a zoomed view are about half as wide (Samuel W7STF)

Zooming filters the signal before re-analysing it, and that filter began rolling off
just inside the edge of what is drawn, so the outer part of a zoomed view was
attenuated — measured at −16.6 / −10.3 / −8.0 dB at ×2 / ×4 / ×8. Samuel's own estimate
of the affected width was accurate. It has behaved this way since zoom was added.

The filter is now twice as long (31 → 63 taps), which roughly halves the width of the
darkened band. Widening it instead was rejected deliberately: that trades a dark edge
for energy above Nyquist aliasing back in as **false signals**, which is worse. A dark
edge is honest.

Cost was measured rather than assumed, because this runs on the task that is the audio
ring's sole consumer: 1.95–2.02 ms per window at ×2 against a 21.3 ms window period,
1.10 ms at ×4, 0.64 ms at ×8. An A/B against zoom ×1 confirmed it lands on core 1
(84–90% idle → 56–85%), not on core 0.

### Out of band, the band strip is a coarse tuner (Samuel W7STF)

v1.8.2 made the strip stay visible out of band instead of leaving an empty row, and his
fair objection was that it then earned nothing — `Band: ---` in the top-left already
says as much.

Inside a band the strip is a **map**: where you touch is the frequency you get. Outside
one there is nothing to map, so it works the other way round. A handle sits in the
middle; drag it off centre and the dial moves, let go and it springs back. A full pull
to either edge moves by **half of what is on screen**, so two drags overlap instead of
skipping a gap, and it gets finer as you zoom in — his "contiguous as we scroll".

A plain tap out of band deliberately does nothing: with no band plan behind it, a tapped
position has no frequency to mean. In band, tap-to-position is unchanged.

### The browser's decode list shows distance and bearing (Tony Abbey)

**KM** and **BRG** columns after the audio tone, in the same order the Tab5 uses, with
the header reading **MI** when *Distance in miles* is ticked. The Tab5 works the
distance out and sends it, rather than the browser calculating its own, so the two
screens cannot disagree. Both fields are **omitted entirely** when either grid is
missing and the browser shows a dash — never a distance that cannot be stood behind.

### Also in this release

- New hidden dev action `{"action":"drawer","scroll_y":N}` so a drawer section below the
  fold can be screenshotted instead of its layout being taken on trust.

### Known and not explained

- **Core 0 sits at 0–5% idle in panadapter mode**, where this project's notes record
  ~14–35% historically. Not caused by the longer zoom filter — an A/B against zoom ×1,
  with the filter entirely off, showed the same figure. Not investigated.
- **The `FRAME-MISALIGN` counter added in v1.8.2 has still never fired**, across a full
  session with the radio streaming. That is not a refutation of the phantom-CW
  mirror-image hypothesis — the session ran with no antenna and with the Bluetooth mouse
  off, i.e. none of the load suspected as the trigger — but the cause remains
  **unconfirmed** and should not be described as found.

---

### Shipped in v1.8.2 — 2026-08-13

*(Backfilled 2026-08-16 — this entry and v1.8.3's were missed at the time.)*

- **Spur suppression** (`dsp/spur_map.c`, opt-in, default OFF). The QMX makes its own comb: +38.6 dB at 14.074 with the BNC open. A spur's baseband offset moves at **16–50× the dial**, so a 25 Hz nudge is a physical discriminator rather than a statistical guess. Two modes — subtract the measured power (~12–17 dB, can never hide a real signal) or interpolate the bins away. Cached per frequency, so a revisit needs no nudge. Teal marks under the frequency labels show what is being touched.
- **A GPS-less QMX no longer overwrites a trusted clock.** The old guard was "SNTP is fresh", and offline SNTP is never fresh — so it could not help the one case that needed it. Don WB0LQW arrived at a POTA site, switched the radio on, and its power-on 00:00 replaced his RTC.
- **RIT long-press parks and restores the offset** (Roy KI0ER); the offset is printed on the waterfall; the band strip reads "Out of band" instead of vanishing (Samuel W7STF).
- **The CQ failure path no longer blames the callsign** for an unbuildable message (Don WB0LQW) — and its new log line is what solved the underlying fault the next day.
- **`usb_replug` no longer power-cycles a wedged radio**, which the operator saw as his QMX being switched off.

### Shipped in v1.8.3 — 2026-08-14

*(Backfilled 2026-08-16.)*

A field-report release — every fix in it was reported by a user.

- **QRZ/eQSL credentials can be re-entered** (Brian WA6JFK): the prompt only fired when nothing was stored, so a mistyped key was unreachable.
- **dBm gridlines derived from the dB range** (`util/db_gridlines.c` + host harness). The mutation run found a real `max_n` overrun no test reached.
- **Adaptive-floor slider removed from the drawer** — it could not change anything, because the tracker it fed is re-seeded 17×/s.
- **Passband overlay corrected to the radio's real digital filter, 150–3200 Hz** — and `FW;` reports a *top edge*, not a width, in DiGi. Pixel-measured on `/ss.bmp`.
- **RF gain read-back repaints when the answer lands.** My first version counted its timeout from drawer open and was falsified on hardware, since CAT link-up is ~17 s after boot.
- **Zoom FIR 31 → 63 taps** for the dark zoomed edges; **out-of-band band strip became a centre-detented coarse tuner**; **browser decode list gained KM/BRG** (Tony Abbey), computed on the device so the two screens cannot disagree.

### Shipped in v1.8.4 — 2026-08-16

**The radio's own menus on the Tab5, and a batch of fixes that stop it doing things you did not ask for.**

**Radio menus (#147 — Randy N4OPI, seconded by Michael KZ4LY).** The QMX's own 80×24 terminal, on the Tab5 (Settings → Radio → Radio menus) and in the browser. For a QMX+ with no control panel this is the only way into the menu system at all.
- It runs on the radio's **second** USB serial port — interface 5, which is *not* contiguous with CAT's interface 0 because the audio function occupies 2–4. Measured: CAT frequency, mode and the S-meter keep running for the whole session. Enable it once on the radio: System config → GPS & Ser. ports → USB serial ports → 2.
- Closing walks the radio out through its own *Exit terminal* item, found by **reading the screen** rather than counting keypresses, so it works however deep you are. A two-minute idle watchdog and a browser-close handler are the backstops.
- The screen model (`util/ansi_term.c`) is host-tested against the **real bytes the radio sent**, re-fed at every one of 287 byte-split points. Reverse video is load-bearing: it is the only thing marking the selected item.
- Font is JetBrains Mono (OFL) at size 25 — a correctness constraint, not taste: the highlight is drawn at `col*CELL_W`, so only a size giving a whole-pixel advance works.

**Auto-answer safety (#142–#145 — Roy KI0ER), all four hardware-verified.** It waits until both transmit windows are mapped before its first call; cancelling a transmission also switches it off; a band change switches it off **by any route**; and it is off at every startup. The band case was genuinely broken — the stand-down existed only on the band-button path, so the web page, a spot, a memory recall and the radio's own knob all left it running into an untuned antenna.

**A TX offset chosen mid-QSO is honoured (#151 — Roy KI0ER).** The plumbing was always right; the *refusal* was the bug. A burst covers ~12.6 s of a 15 s slot and a QSO transmits every other slot, so roughly 40% of attempts were rejected outright, leaving the exchange on its starting offset. Now queued and applied the instant the burst ends.

**Spur suppression offers the mode that works first (#157 — Samuel W7STF).** Measured before changing anything: the detector finds 87 bins, strongest 38.5 dB over the floor, exactly as designed. But the menu offered the weaker treatment first — on the waterfall, Subtract is −28% and Erase −78%. Erase now comes first, leaves no dark notches, and its ~3 s learn is cached per frequency.

**USB mouse decoded from its report descriptor (#152 — Kevin KW6E).** A 16-bit X read as 8-bit made the horizontal axis wrap every 256 counts and the vertical axis nearly dead. Deliberately **no** speed setting was added: a scale factor cannot make one axis wrap while the other stands still, so it would have hidden the fault.

**A WiFi hiccup can no longer reboot the device (#131).** esp_hosted's transmit path gave up after two CMD53 timeouts 11 ms apart and restarted the whole P4 rather than drop one frame — a clean `esp_restart()`, so no panic dump and `reset_reason` reads `SW_CPU_RESET`. Now 8 attempts with a pause, then the frame is dropped and the link stays up; the restart survives only behind 32 *consecutive* failures.

**Smaller, all field-reported:**
- **ADIF viewer Close was red** — brighter red than "Delete all" (Gyula HA3HZ). Now neutral; in that panel red means only "this deletes your log".
- **A QSO that could not be written was reported as logged** — found while answering Gyula's question about log capacity. The write path checked neither `fprintf` nor `fclose`. It now refuses to count a contact it could not save and names the station on screen.
- **A top-bar label that lost the display lock stayed wrong forever** — the caller only fired on change, so one missed lock left the screen disagreeing with the radio indefinitely.
- **`ft8_status_set()` aborted the device if called before init** — `xSemaphoreTake(NULL)` is an immediate abort, and the FT8 screen could reach it while the QMX-wait prompt was up.

**Standing patch #6** — `tools/patches/apply_cdc_acm_close_tolerant.ps1`. `cdc_acm_host_close()` fed `usb_host_interface_release()` into `ESP_ERROR_CHECK` while allowing the client task only 10 ms to reap URBs, so a busy port turned a transient into a reboot. Retries the release, then logs and continues.

⚠ **Not verified in this release:** Kevin's mouse (no Surface Arc here — the new report-map log line will settle it), the ADIF Close colour on screen, the failed-write path (needs a full filesystem), and the SDIO drop path (needs a soak). **#118, the phantom CW mirror, remains open** — the IQ-mode explanation was falsified on hardware in Roy's exact configuration.

### Shipped in v1.8.5 — 2026-08-17

**Mostly the things people were already told were fixed, plus a batch more found the same evening — including a WiFi fault that turned out to be three patches quietly missing from the build.**

Six of these had been described as done in replies posted before v1.8.4 went out, which meant anyone who took those replies at face value went looking for them in a build that did not contain them. That is the main reason this release exists.

**Radio menus: you can now see what you are typing (#161, #162, #163 — Randy N4OPI, Michael KZ4LY).**
- **A block cursor is drawn.** It was tracked internally the whole time and simply never rendered, so editing a Messages field meant guessing where you were.
- **BS deletes leftward, and it took two goes to get right.** Randy N4OPI answered the open question from PuTTY — *"landing in a numerical field puts the cursor at the right most digit, Backspace deletes leftward and then you can type in the desired values, Del does nothing"* — and v1.8.5 first shipped that as 0x08, the byte a Backspace key "obviously" sends. It does nothing on the radio. **PuTTY's Backspace sends 0x7F by default**, so Randy's Backspace was 0x7F all along and his "Del" was PuTTY's Delete key sending an escape sequence. Every word he wrote was accurate; the translation into a byte was not. Measured on the radio with ▲▼ as a control to prove the key path worked: 0x08, ◀, ▶ and Enter do **nothing**, while 0x7F turns `11.5` into `11.` into `11`, and typing then appends. There is now one key, labelled **BS**, sending 0x7F.
- **An on-screen QWERTY**, on a keyboard button in the top row. It types straight into whatever field the radio has open — there is no local text box, because the radio's own menu owns the field and its cursor. Styled like every other keyboard in the app, and every button in the header row is now the same size, both after the operator used it and said so. *(Randy N4OPI, Michael KZ4LY)*
- **The fields that "would not increment" were never an increment problem.** Values longer than two digits, and values in tables, are backspace-and-retype; the arrows moving between columns in a table is the radio's own intended behaviour. So Randy's whole list — Max. PA voltage, the band config columns, CAT timeout, TCXO, the Virtual U3S values — is explained by one wrong byte rather than a missing feature.
- **Correcting a report by hand.** The web log viewer is called *View / edit log* and could not edit anything, only delete. The two report columns are now click-to-edit, and leaving one empty records that no report was exchanged. Deliberately narrow: only the reports can change, because callsign, band, mode, date and time are what QRZ, eQSL and LoTW match a contact on. An edit corrects the Tab5's log only — a copy a logbook already holds cannot be amended by re-uploading, and the page says so. *(Gyula HA3HZ)*
- **Reports are logged only if they were transmitted.** `RST_SENT` was recorded when a message was *armed*, and an armed message can be replaced before it reaches the air — which happens deliberately whenever the partner is re-heard with a fresher signal. So the log could carry a report that never went out, which is the same dishonesty as the fabricated `599` removed in v1.3.4. It is now latched from the burst that actually fired. *(Gyula HA3HZ)*
- **Leaving Radio menus hands the radio back properly.** Roy KI0ER found the waterfall misbehaving after a menu visit, cleared only by pausing and resuming by hand — which is the tell, because that path re-runs the IQ handshake and re-seeds the noise floor, and closing the terminal did neither. A QMX's IQ mode is session state that a trip through its own menus can drop.
- **The station you are working is never hidden by a display filter.** With *Show only CQ callers* on, an exchange with your own callsign vanished from the list — two filters were stacking, one hiding other people's CQs during a CQ run and one hiding everything that is not a CQ. The contact in progress is now exempt from all of them. *(Roy KI0ER)*
- **A pileup caller is answered from what they actually sent.** The automatic drain always sent a bare report, which is right for someone who sent a grid and wrong for someone who reported you. It now builds through the same code the manual **Transmit** button uses — the third and last instance of a bug family where the manual path was right and the automatic one lagged.
- **"Exit terminal" no longer re-opens the session.** This was mine: choosing Exit clears the screen, and a recovery meant for a lost opening character saw a blank screen and helpfully woke the radio back up. It is now bounded to the first 8 s after opening, where it belongs.
- **The menu path is on screen** when the radio has no second port — `System config → GPS & Ser. ports → USB serial ports → 2`, in both the Tab5 panel and the browser, rather than a toast that disappears. Michael's point was that the person needing that instruction is the one who never read the announcement.
- Also fixed while testing: the code that finds the highlighted row **skipped rows 0–1**, which is right for the main menu but wrong three boxes deep, where a submenu's own title sits on row 2 — so inside a submenu it returned the *title* instead of the selection. It now discriminates on the box border itself, which holds at any depth. It had never bitten because the exit walk sends Ctrl-Q three times before looking, i.e. it was correct by accident.

⚠ **Still not fixed, deliberately:** values over two digits, and values in a table, do not increment with ◀ ▶ — Max PA Voltage, the band-config columns, CAT timeout, TCXO, the U3S fields. Randy's own pattern is the lead. These need some key other than left/right, and the question has gone to QRP Labs rather than a guess going into a release.

**The clock no longer claims GPS it does not have (#173).** A QMX with no GPS at all was showing `UTC(GPS)`. The detector confirms GPS when the radio's second-tick agrees with SNTP inside 300 ms, and its reasoning said a clock the Tab5 had pushed could never pass, being "only whole-second accurate". It is not: the time is sent at whatever instant the call happens and the radio starts its second when it parses, so the phase we induce lands **anywhere in 0..1000 ms** — measured across successive pushes at **12 ms, 834 ms, 154 ms and 404 ms**. The same code confirmed GPS or not purely on the draw.

It was not just a wrong label. Once confirmed, the Tab5 **stops maintaining that radio's clock** — and a QMX's clock is not kept across a power cycle, so the one radio that needed the correction stopped getting it. The verdict also persisted, so an offline POTA session inherited it and began trusting a free-running RTC as authoritative. A clock the Tab5 has set is now never accepted as evidence about itself; the flag clears when the radio's clock is plainly its own again. Both branches were verified on hardware, including the exact case that used to lie: `agrees to 154ms, but WE set this radio's clock - not treating that as GPS`.

**The CW display follows the offset you actually set (#165 — Roy KI0ER).** The dial agreed with the radio but the waterfall did not, and tapping a CW signal tuned about 30 Hz off — so he transmitted off frequency as if XIT were on. The radio's CW offset was read in exactly **one** place, the one-time link-up sequence, and never again; the moment he changed it on the radio, the display's compensation froze at the link-up value for the rest of the session. A stale constant is what "not linear in the value I asked for" looks like, which is why his own formula had to be withdrawn. It is now re-read every 5 s while in CW, and nothing at all in any other mode. Verified by changing the radio under a running session and watching the value follow 700 → 650 → 800.

Measured first rather than assumed, because the approach depended on both: setting CW centre to 650 drags **both** CW offset and sidetone to 650 (so that is the right item to read, and it does track the centre), and repeated reads are stable with no `FW;`-style re-assert side effect — which is what makes polling it safe at all.

**A caller who answers your CQ with a report is followed (#167 — Gyula HA3HZ).** An operator who already knows they have you often skips the grid and reports you straight away. The correct answer is `R` plus your report, which acknowledges theirs and gives yours in one message; the automatic run sent another bare report instead and lost a cycle. The information was already there — the same function detected this case a few lines earlier to capture their report for the log, and the reply branch just did not use it. Tapping **Transmit** by hand always did the right thing, which makes this the second of three instances of the same manual-right/automatic-wrong split; the third was found by grepping for the class and is not bodged.

**Daily ADIF export with the date in the filename (#170 — Gyula HA3HZ).** A **Today only, dated file** link under the ADIF download gives that day's contacts as `qso-YYYY-MM-DD.adi`, so a daily file is self-identifying once saved instead of something to rename by hand.

**The red transmitting banner no longer covers the text under it (#166 — Gyula HA3HZ).** Measured in a browser rather than eyeballed, and worse than it looked: a 99-pixel panel holding 406 pixels of content with nothing to contain it. It can now shrink and scroll, and the status line no longer wraps to three lines.

⚠ **Not verified in this release:** the menu path on screen has not been *seen* — it needs a radio with the second port disabled; and Gyula's banner fix is verified by measurement in a browser, not against his screenshot. Roy's CW residual is also unsettled: a single stale value predicts an error of *requested − stale*, and his 30/40/140 would need three different stale values, so either he rebooted between tests or a second component remains — his own IF-calibration trim is the obvious candidate. He has been asked to retest rather than have three hand-read points fitted to a curve.

### Shipped in v1.8.6 — 2026-08-18

**A same-day fix release. v1.8.5 shipped with the browser interface completely dead, and that is the headline.**

**The web UI works again (#183 — Randy N4OPI, Michael KZ4LY).** One unterminated string literal in the page stopped the *entire* script running, so the browser drew its controls and then did nothing at all: no spectrum, no waterfall, no working buttons, "disconnected" in the corner, in both Chrome and Firefox. Anyone who used the browser had nothing.

It was not a typo. The literal was written by a script whose shell collapsed a `
` into a real newline before it was ever saved — the same collapsing that produced a NUL byte in one C file and a stray control byte in another on the same evening. The compiler caught both of those instantly; nothing caught this one, because the firmware build had no reason to parse HTML. **It does now**: the build extracts the page's script and refuses to compile if it does not parse, and that check was tested by re-breaking the literal exactly the way it shipped.

**A crash that was blamed on the radio (#182).** An overnight soak of v1.8.5 aborted at 1 h 57 m of a completely healthy FT8 session on `usb_dwc_hal.c`'s assertion that "an error should have halted the channel" — an assumption ESP32-P4 silicon does not honour. The reboot is the cheap part: an abort is a warm reset with the QMX attached, which is the one condition that leaves the radio unable to re-enumerate, so it then stayed dead for the remaining 5 h 26 m. The morning's report was "the QMX wedged during the night". The QMX was fine. This is now **standing patch #7**, and it reports the error instead of aborting — the recovery path was already written directly beneath the assertion.

**CW frequency display and tap-to-tune (#165 — Roy KI0ER).** A signal transmitted on 7.060.000 showed at 7.060.040, and tapping it tuned him 40 Hz off, so the far station heard him shifted. Two separate faults added up to that figure:

- The per-unit CW trim defaulted to **−60 Hz**. It arrived with the commit that first read the CW offset from the radio over CAT, so it was calibrated *before* that reading existed and never revisited. Worth a flat +60 Hz.
- The display offset was **rounded to a whole FFT bin**. A bin is 46.88 Hz, so that alone can misplace everything by up to 23 Hz — and by an amount that changes with the CW offset you choose, which is why the errors looked non-linear in the value set.

The trim now defaults to 0 (the slider stays, for real per-unit trimming) and the rounding remainder is compensated where it matters: a tap converts screen position to frequency correctly, and the dial marker is drawn on the signal rather than beside it. The arithmetic reproduces **both** of Roy's measurements — 650 Hz predicts +41, 700 Hz predicts +44, against his reported +40 — and both go to zero.

✅ **CONFIRMED ON A REAL SIGNAL** (Roy KI0ER, 2026-08-18, v1.8.6 + QMX 1.04.007): *"an incoming CW signal's peak on the waterfall aligns properly with the actual frequency now. If it is off a tad, it appears it's within 5 Hz."* Tested at his 650 Hz CW centre; other centre/offset/tone settings not re-tested. This shipped verified only by arithmetic matching his earlier numbers - the measurement has now caught up with it.

**Radio menus, all from Michael KZ4LY using it:**

- **You can see what you are typing past message 9.** The keyboard covers the lower half of the screen, so tall menus were edited blind. He suggested making the keyboard transparent; the screen now scrolls instead so the radio's cursor row stays above it — two layers of overlapping text are readable only if you already know what they say.
- **The no-second-port help says to power-cycle the radio.** He put the naive-user hat on deliberately and found the setting alone is not enough.
- **The two-finger blank actually works.** It was succeeding fewer than one try in ten, with the tune cursor stealing the gesture. Two fingers never leave the glass on the same instant, so a normal lift was measured as lasting until the *second* finger left — over the time limit nearly every time — and the finger left behind looked like a deliberate one-finger touch to both the pan handler and the tuning handler. ⚠ Not verified here; it needs two real fingers.

**Also documented (Stan Dye KC7XE):** in the band config table the Enable/Disable entries accept **E** and **D** as characters, and because those fields use the arrows to change the value, the arrows will not move you between columns while you are on one — step onto a numeric column first. That is the radio's own behaviour, and it explains something that otherwise reads as the arrows being broken in tables.

**Not a bug (Brian WA6JFK, answered by Roy KI0ER):** auto-answer is off after every restart by design. A radio that began transmitting the moment the Tab5 powered on might be feeding an antenna that is not tuned yet. WSJT-X requires arming transmit per startup for the same reason.

---

## v1.8.7 — 2026-08-19

**The browser panadapter stops freezing, your logs can go to your own Cloudlog, and the radio's menus show the radio's colours.**

### The web panadapter freezing for seconds at a time (Samuel W7STF, #177196)

His report was that it *"hangs from time to time, and then several seconds later it begins to animate again"*, and that it had been getting worse with every release.

Measured before anything was changed, because every "obvious cause" in this project's history has been wrong when measured. Over 9.6 hours on the bench the browser session was torn down **545 times**, roughly every 14 seconds in bursts, each costing a **median 2.2 seconds** of frozen display while the browser reconnected, worst case 4.5 seconds. Between the drops the stream ran at its full 10 frames per second. Feed traffic was heavily implicated — RBN 8.4x and the DX cluster 8.6x more likely in the two seconds before a teardown than at a random moment. Socket exhaustion was tested and ruled out.

But the feeds were the trigger, not the fault. **ESP-IDF's WebSocket send writes the frame with a single call and only treats a negative result as an error — and a partial write is not negative.** A frame that went out half finished was reported as sent. The browser then read the next frame's header as the tail of the previous one, saw a protocol violation, and closed the connection. That is why it froze for seconds rather than glitching one frame, and why it got worse as more background feeds were added: more congestion, more partial writes.

The July fix that stopped a stuck send freezing the whole web server made it more likely. That fix put a 400 ms timeout on the socket, and a send timeout with bytes already queued is exactly how a short write happens. **The freeze fix and this bug were the same line of code.**

Now the send loops until every byte is out, the frame header is built in front of the payload so the two cannot be split, and a frame that cannot be finished closes the session rather than leaving the browser reading rubbish. Congestion with nothing yet sent simply drops a frame, which costs nothing. The repairs are counted and reported, so the fix can be seen working rather than merely assumed.

**Verified on hardware:** 9.3 minutes under live feed load gave **0 teardowns** where the old build predicted about 9, **0 connection resets** where the old build logged over a thousand, and **8 partial writes caught and completed** — each one a stream that would previously have been corrupted.

### Upload to your own Cloudlog or Wavelog (Mark G4MEM)

The fourth logbook, and the only self-hosted one. Because the address is yours rather than compiled in, this is the first time the firmware sends credentials somewhere it does not already know.

Mark runs his on his home network with no certificate, and asked whether plain `http://` could be allowed when the server is on the same subnet the Tab5 is connected to. It can, and it is. That is not the same as an "ignore the certificate" switch, which was refused: the subnet test is a fact the firmware can check, and on your own network the packets never leave equipment you own.

Two deliberate limits. The check runs **on every upload, never cached at setup**, because the whole point is that you configure at home and then operate from a field site where the same name could be answered by anything. And plain HTTP needs a numeric address, since a name has to be looked up and what answers the lookup can differ from what answers the connection. Use `https://` for a hostname.

Records go in batches, Cloudlog does its own duplicate checking, and Wavelog works identically. If your server is on your own network this is the only upload that needs no internet at all, which suits POTA and SOTA better than the other three.

### The radio's menus in the radio's colours (Samuel W7STF)

Radio menus rendered everything white while PuTTY showed the same screens in red and green. The colour was being parsed and stored all along; both renderers simply discarded it. Fixed on the Tab5 and in the browser, from the same palette so the two cannot disagree. The selected-item highlight still wins wherever both apply, since losing it would cost you your place in the menus.

### Battery showing 100% then 0% with no battery fitted (Randy N4OPI)

On a Tab5 run from USB-C with no NP-F550, the readout alternated between 100% (8.4 V) and 0% (4.2 V). A no-battery detector already existed and was flapping in time with the rail rather than latching: it watched a five-sample window, so while the voltage sat at either value the window looked perfectly steady and the detector decided a pack was present again.

It now also checks two things a real pack cannot do — moving several volts between one-second readings, and running the device at 4.2 V. The web page is told as well, so it shows "USB" instead of inventing a percentage.

### Pick callers myself, while running CQ (Eric K3FNB)

> "When I am activating a park, I sometimes like to be a bit more engaged. If it is possible, could you have an option where I have to tap on a caller/hunter in order to initiate the exchange? I don't mind the firmware automating the rest of the exchange, I just would like to have the option to not auto-pounce on hunters."

New option in the FT8 Filter modal. With it on, a station answering your CQ does not start the exchange — they wait in the pile-up until you tap them, and the exchange then runs itself exactly as before. Only the *choice* becomes manual.

It deliberately keeps calling CQ rather than standing down. An activator wants the pile-up building while they pick, and a radio that went quiet would look like it had stopped. It also overrides "Auto-work pileup", because both settings decide who to work next and someone who asked to choose has not asked to have the strongest caller chosen for them a moment later.

Off unless you turn it on.

⚠ Verified in simulation mode, both ways — with the option on, a caller waits in the pile-up and no exchange starts; with it off, the same caller is answered automatically. Not yet used on the air, so if it misbehaves during a real activation please say so.

### A Bluetooth mouse whose pointer moved erratically (Kevin KW6E)

His Microsoft Surface Arc connected and scrolled perfectly but the pointer jumped about. Two things were wrong. The mouse fix that shipped in v1.8.4 went into the **USB** path, and his mouse is **Bluetooth** — so it never touched the code he was running.

The rest came out of the diagnostic log he attached, without needing his hardware. His mouse sends nine-byte reports with 16-bit movement; the firmware treated any report of five bytes or more as the layout of a Logitech M240, where the two axes share a nibble. His own report `00 06 00 0b 00 ff ff 00 00` is X=+6, Y=+11, and that arithmetic turned it into X=−1280, Y=0 — a large jump the wrong way with no vertical movement. Scrolling survived because the wheel byte lands in the same place either way.

The decode now picks between layouts that have each been captured off real hardware, and the same routine serves both the USB and Bluetooth paths — the USB one had the matching assumption waiting for the next mouse. The mouse's own report descriptor remains the preferred answer; this is only what happens when it cannot be read.

### A warning that blamed the wrong thing (Samuel W7STF)

After swapping cables he once got "is the radio set to 2 USB serial ports?" on a radio that was already set to two. The second port was opened exactly once, and any failure produced that message — so a radio still re-enumerating after a cable swap was reported as a configuration mistake. It now retries, and only names the setting when the radio is demonstrably present.

### Also in this release

- **A radio receiving on VFO B is put back on A**, and told you about it (Markus DL8MBY). Frequency writes go to VFO A only, so with the radio on B every tune went to a VFO he was not listening to while band select still worked — which is exactly why it looks like your own mistake rather than a fault.
- **Waking from the screensaver no longer acts on the tap that woke it** (Randy N4OPI). Sleep disabled the mouse pointer and left the touchscreen live.
- **The browser's spot and frequency labels are readable on a high-resolution display** (Randy N4OPI). They were being drawn at roughly half size.
- **A crash after about seven hours is fixed** — a bare `abort()` in ESP-IDF's USB driver on a state it treats as impossible. This is the fourth of that family. As with the last one the reboot was not the expensive part: it happens with the radio attached, so the QMX then could not reconnect for the rest of the night.
- **The flash-persisted diagnostic log no longer stops writing.** It reported "no space left" with 400 KB free — not a capacity problem but a garbage-collection one, caused by deleting a 256 KB file every 11 minutes. The boot log now lists every file with its size.
- **Two silent USB patches now count what they catch.** They cannot log, because they run in an interrupt, so a clean log could not distinguish "never happened" from "happened and was handled". Neither has fired yet, which is now a statement of fact rather than an inference from silence.
- **Entering FT8 could reboot the device.** All the decoder's monitors share one FFT scratch buffer, and the code that set that up freed each monitor's old buffer without checking whether it was already the shared one — so if the pool was ever built twice without being torn down in between, the same block was freed twice and the device rebooted. Caught on the bench while testing this release, and it is almost certainly the unexplained heap crash that had been on the list since v1.3.0. The reboot is fixed; the underlying double build is not, and now logs a loud warning instead of being survived in silence.

### Investigated, no change

**The first entry into Radio menus after a flash drawing blank** (Samuel W7STF) could not be reproduced: the first open after flashing plus six more were all clean. Randy N4OPI sees the same thing in PuTTY, which points at the radio's own redraw rather than at the Tab5.

---

*This is the archived "Shipped in" history. The live roadmap (Next up / Longer term) is in [`README.md`](../README.md).*


## v1.8.8 — 2026-08-20

Diagnostics + field reports.

- **#117 a crash now survives the reboot.** `util/panic_hook.c` wraps
  `esp_panic_handler` (`-Wl,--wrap=`) and stashes a record in RTC no-init RAM;
  the next boot logs it through the ordinary path, so it reaches `/api/log` AND
  `/api/log/saved` with reason, assert text, task name, uptime, registers and an
  `addr2line` line. Deliberately NOT a flash write from panic context — that can
  hang instead of rebooting. A valid record is also positive proof the reset was
  a real crash, which `reset_reason` never was here. Hardware-verified end to
  end: the wrap confirmed in the disassembly, and a deliberate crash decoded to
  the exact `abort()` line. Dev action `{"action":"panic_test"}` keeps it
  falsifiable (radio-OFF only — a panic reboot is the #74 trigger).
- **#199 the FT8 monitor pool could be built twice — root cause found.** The
  liveness flag was set inside the task, so it meant "has begun running" while
  both readers needed "exists"; these tasks are the lowest priority on the board,
  so the watchdog could look during the gap and spawn a second. Almost certainly
  the `tlsf_free` double-free open since v1.3.0. Fixed by claiming the slot
  before `xTaskCreate`; a sweep found a second instance (the decode task) and one
  false positive left alone.
- **record_decode dropped 0.18 % of decodes** (99 of 54,142, measured). Cause was
  CPU starvation, not lock contention — the holder went ~190 ms with no CPU,
  visible as an out-of-order log timestamp. Writer timeout 50 ms → 2 s; readers
  unchanged.
- **#214 BW label stuck on a CW filter after CW → LSB** (Samuel W7STF). The SSB
  pin is never cleared, so `FW;` stays suppressed on re-entry to SSB and nothing
  repainted the label. Repaint from the pin. Hardware-verified.
- **#216 out-of-band tuner on the web UI** (Samuel W7STF). Mirrors `ui.c`:
  centre-detented, full deflection = half the visible span, deflection measured
  from where the pointer landed, tap does nothing. Browser-verified.
- **#217 the takeover notice was a MALFORMED websocket frame.** RFC 6455 §5.2
  requires the minimal length encoding; the 1-byte notice used the 16-bit form,
  so browsers failed the connection instead of standing down and took the view
  straight back. Measured 16 takeovers/10 s → 2/25 s.
- **#215 made measurable, not guessed.** Colour was never missing (per-cell `fg`
  since day one, shipped on both screens in v1.8.7); the parser implements only
  0/7/27/30-37 and the Diagnostics screen sends more. Unhandled SGR codes are now
  reported via `/api/term` as `unk`. Harness mutation-tested. Needs one reading
  from a radio with two serial ports enabled.
- **Frequency label could stick.** A dropped `display_lock` write was permanent
  because the FA path is change-gated; the poll now re-asserts it.
- **#154 mutex-init audit** — `dsp.c` had six unguarded takes with `ui_init()` at
  main.c:202 and `dsp_init()` at 312; `ft8_greylist.c` had an unguarded site and
  a lazy-create race across two tasks. Three other files verified safe.
- **WS health counters** in `/api/status`.

## v1.8.9 — 2026-08-20

- **#146 the radio could be left transmitting, and now cannot.** Roy KI0ER's log
  is the whole mechanism: `cdc_acm: TX transfer timeout` mid-burst, then every
  write returning `0x10c`, so **both** `TA0;` and `RX;` were lost; the burst
  declared itself complete and the QMX transmitted until he power-cycled it,
  with the Tab5 UI normal throughout. ⚠ **The link recovered ~2 s later and
  nothing re-sent `RX;`** — which is why a burst-local retry (8 x 20 ms) is not
  sufficient on its own. `tx_cmd_critical()` retries the stop commands, and on
  failure `cat_request_force_rx()` hands it to the poll task, which re-sends
  `RX;` on every cycle whose send SUCCEEDS (the proof the pipe is alive) until
  it gets through. The poll task owns the pipe and is the first code to learn
  the link is back; ft8_tx has no business blocking a slot boundary for seconds.
- **#218 one-tap firmware update.** Three 4 MB app slots with `user_nvs` and
  `storage` at unchanged offsets and `factory` kept as a permanent known-good
  fallback; rollback enabled; refuses while transmitting or mid-QSO. Long-press
  to confirm ("release to confirm"), never automatic — an OTA reboot is a warm
  reset, i.e. the #74 trigger. Tab5 and browser share one vocabulary. Awareness
  was the bigger half: the update check had run since v1.1 with **zero callers**
  outside its own module, announcing itself only inside the Reader.
- **#117 crash records survive the reboot** (RTC no-init RAM, `-Wl,--wrap`), so
  a field crash is diagnosable from a diagnostic download for the first time.
- **#153 the SD diagnostic log continues while WiFi is on.** Previously a boot
  snapshot only — proven from Roy's own log, where `qmx-log.txt` never appears.
- **#220 flasher 44 MB -> 2.9 MB** (Gyula HA3HZ). The zip was built with a
  wildcard over a directory holding 27 historical flasher archives.
  `tools/make_flasher_zip.ps1` now packs an explicit list.
- **#219 mid-QSO tone relocation, nearest slot only.** Roy KI0ER established
  that decoders match on callsign not tone, so the old "never mid-exchange"
  justification was false; Gyula HA3HZ established the limit, that a narrowed
  receive window may not hear a big jump. Capped at 250 Hz and 3 moves.
- **#214 BW stuck on a CW filter after CW -> LSB** (Samuel W7STF): the SSB pin is
  never cleared, so `FW;` stays suppressed on re-entry and nothing repainted.
- **#216 out-of-band tuner in the browser**, mirroring ui.c exactly.
- **#217 browser stalls**: the takeover notice used the 16-bit length form for a
  1-byte payload, violating RFC 6455's minimal-encoding rule, so browsers failed
  the connection instead of standing down. 16 takeovers/10 s -> 2/25 s.
- **#222 spur suppression parked.** `dsp_get_zoom_spectrum()` bypasses
  `spur_map_apply()`, so it only ever worked at zoom x1. Code kept, drawer row
  removed. With an antenna the strongest tooth is 22.7 dB over the floor against
  38.6 dB with the BNC open.
- **#221 API audit**: 37 actions documented (14 were), and the error semantics
  corrected — `/api/cmd` answers an unknown action with **HTTP 200**, not 400.
- Frequency label re-asserted from the FA poll; `#154` mutex-init audit;
  `#199` FT8 monitor-pool double-build root-caused.

### Shipped in v1.9.0 — 2026-08-21

**The web page is fast again, the snap-on keyboard drives the whole app, and
updating from the device can finally be seen working.**

**Web UI performance — three separate faults, all measured rather than guessed.**

- **The page was served UNCOMPRESSED**: 263 KB with `Cache-Control: no-store`,
  so every visit re-downloaded all of it, on a link the 10 fps spectrum stream
  already shares. Now gzipped at build time to 83 KB (3.2x) with an **ETag =
  the running firmware's ELF hash**, so a reload of unchanged firmware is a 304.
  A cold load went **~95 s → 1.5 s**. The compression is a CMake step, not a
  committed artifact — `main/manual.bin` is the cautionary tale of a derived
  file shipping stale.
- **`esp_wifi_set_ps()` was never called**, so WiFi ran at IDF's default
  `WIFI_PS_MIN_MODEM`. Outbound traffic was unaffected (the spot feeds always
  worked), while inbound depended on the AP buffering for a sleeping radio.
  Measured over 494/252 samples: HTTP failures **14.4% → 0.4%**, TCP **15.2% →
  0.8%**, p90 latency **1137 ms → 543 ms**, with the router as a 0%-failure
  control throughout. Now `WIFI_PS_NONE`, set from the `STA_START` handler so
  all four `esp_wifi_start()` call sites are covered by one line.
- **⛔ The page was evicting the spectrum WebSocket.** `httpd` closes its
  least-recently-used session when the socket table fills, and
  `httpd_sess.c` refreshes `lru_counter` in exactly ONE place — when a request
  is **received** on that socket. `httpd_sess_update_lru_counter()` is a public
  helper the component never calls. Our WS only ever *sends* and deliberately
  never receives, so its position froze at the handshake and it was permanently
  bottom of the pile; the browser's own `/api/status` and `/api/decodes` polling
  was enough to get it purged. Measured session lifetimes **~10 s → ~28 s** from
  the one-line fix, plus the counter refreshed while paused and
  `max_open_sockets` 10 → 13.

⚠ **An adaptive WS frame rate was built and REMOVED the same day.** Its premise
was a "~12.7 KB/s link" measured while the page was still uncompressed and power
save still on; re-measured afterwards, six identical 86 KB fetches ran at min
9.9 / median 32.5 / max 74.7 KB/s. A **7.5x spread on identical work** means the
link is LOSSY, not narrow, and rate control cannot help that. Its own telemetry
confirmed it never engaged. The reasoning is kept as a comment in
`webserver_ws.c` — measure the spread before reaching for it again.

**The Tab5 snap-on keyboard (#233).**

- Enter/Esc wired into the five modals that had buttons and no keyboard: FT8 TX
  confirm, Antenna Tune (Esc only — it keys a carrier), the Reader, the guidance
  panel and onboarding. Esc also backs out of the memory page and the drawer,
  neither of which has a Cancel button.
- ⛔ **A latent bug found on the way**: `ui_kbd_set_buttons()` stored ONE pair,
  but these modals are built at boot and shown/hidden rather than created and
  destroyed — so the LAST MODAL BUILT owned Enter and Esc for the whole session,
  including while hidden. It is a registry now, and dispatch picks the entry
  whose button is actually visible.
- Arrows and PgUp/PgDn scroll the drawer and the manual, accelerating on
  repeated presses. **True long-press repeat is impossible**: String mode reports
  characters, never press/release, and holding a key produced two events 765 ms
  apart. Normal mode (reg 0x20, bit7) would give key-up at the cost of
  reimplementing the whole keymap — deliberately not taken.
- **Shortcuts, built on measured bytes.** The driver had been receiving a
  modifier byte with every keystroke and discarding it. Measured: **Ctrl = 0x01,
  Alt = 0x04**, key in the first byte, payload `[char, modifier]`; Sym and Aa are
  applied inside the keyboard's MCU. 25 actions, 9 bound by default, all Ctrl,
  with Alt left free. **Nothing that transmits can be bound** — a chord may open
  something, never key the radio.
- **User-defined shortcuts**: NVS-persisted, edited from the web page. A binding
  stores an ACTION ID, never a table position — appending an action is safe,
  renumbering one silently repoints somebody's saved shortcut.

**Update checking (#218).** 6 h → **30 min**, plus `update_check_now()` reachable
three ways: long-press the Tab5 version (which did nothing when up to date —
i.e. nearly always), tap it in the web page, or `/api/cmd
{"action":"update_check"}`. The check task sleeps in 500 ms slices so a forced
check does not wait out the interval. The **GitHub fallback is rate-limited to
hourly** and keeps its WS pause — measured at 48,764 B and 3.7 s against
latest.json's **139 B and 0.09 s**, which is why the small check no longer
pauses the stream at all.

**Field reports.**

- **Randy N4OPI ×5.** TX power/SWR hidden behind the exchange status — that
  whole strip rebuilt to lay out ACROSS rather than down, with the figures shown
  only while transmitting. TX tone picker off by 1–2 slots (the draw reserved
  34 px for the E/O letters, the click handler did not; error 0 at the left edge
  and 1.83 slots at the right). "Busy: working …" and "QSO cancelled" never
  clearing (a command outcome rendered as current state; now expires in 20 s).
  A worked station staying green (`get_pinned_call()` gated on `s_target[0]`,
  which deliberately survives completion for the final re-send; both accessors
  now share one predicate — verified through a real QSO, released 84 ms after
  the ADIF write). And **"Who is hearing me" silently truncating** at a 16 KB
  response buffer and a 64-report cap, neither logged — now 64 KB/128 with both
  limits reported, and **reproduced on the bench** hours later at exactly 128.
- **COUNTRY replaces GRID** in the web decode list, spelled out.
- **Don N2VGU**: the keyboard in Radio Menus, above.
- Confirmation dialogs removed from ordinary operating actions (Call CQ, mid-QSO
  Cancel, working a station, Antenna Tune). Destructive actions still ask.
- Mid-QSO buttons equalised; the bottom-bar version colour 0x808080 → 0xC0C0C0.

### Shipped in v1.9.1 — 2026-08-21

**A hotfix for the OTA download path announced in v1.9.0. It never worked
against a real download, on any version, ever, until this fix.**

Two bugs, both in the same `esp_http_client_config_t` block, both found on
hardware in the FIRST real attempt to download an actual release over OTA
since the feature shipped in v1.8.9.

- **The connection could never open.** `esp_http_client`'s response buffer
  defaults to 512 bytes; `github.com`'s own 302 redirect carries 5,159 bytes
  of headers (measured with `curl -I` against the real release URL, dominated
  by a large Content-Security-Policy header). Every download failed instantly
  with "could not reach the download (0xffffffff)" — the offer in v1.9.0
  worked correctly, the download it pointed at could not. Fixed with
  `buffer_size = 8192`. That alone was not enough: `http_client_prepare_first_line()`
  builds the outgoing request line into a SEPARATE buffer sized by
  `buffer_size_tx`, and after the redirect the request's own path+query for
  the second hop IS the entire signed CDN URL (~930 bytes, also measured) —
  overflowing a 512-byte tx buffer on the send side. Fixed with
  `buffer_size_tx = 8192`.
- **Once that was fixed, a genuine hardware watchdog reset appeared right at
  100%.** `esp_https_ota.c` sizes its own per-call image chunk as
  `MAX(http_config->buffer_size, DEFAULT_OTA_BUF_SIZE)`, so the same fix also
  made every download chunk 8192 bytes instead of a few hundred — ~400
  continuous read+decrypt+flash-write bursts back to back over a 3.2 MB
  image, with no yield point anywhere in the loop. Continuous, severe audio-ring
  overflow (tens of thousands of samples/s dropped, every second) ran for the
  entire download, escalating to `rst:0x7 (HP_SYS_HP_WDT_RESET)` — the same
  "interrupts/cache disabled too long" mechanism this board's documented
  cyan-flash bug already describes, sustained for minutes instead of one
  frame. Fixed with one `vTaskDelay(1)` per chunk.

**Verified on hardware, twice, against the real published release asset**: a
full download completed 0 → 100% in ~4m18s with zero crashes and zero audio
drops for the entire capture, printing the code's own success line. A second
run confirmed the fix survives a fresh boot.

⚠ **Consequence for already-deployed units**: this is client-side, so no
future release can OTA-update a unit that does not already have this fix — its
own currently-running code performs the (broken) download regardless of
target version. Every v1.8.9/v1.9.0 unit needs one cable flash to reach code
that can OTA at all; every update after that works normally.

⚠ Also recorded for the release process: `esp_https_ota_finish()` calls
`esp_ota_set_boot_partition()` unconditionally on success, so a bench OTA test
against a partially-fixed build silently repoints the next boot at the
just-downloaded (possibly older/unfixed) image via `otadata` — a plain reflash
of `factory` does not undo this. Recovery: `esptool erase_region <otadata
offset> <size>`, which returns it to blank and falls back to `factory`.

### Shipped in v1.9.2 — 2026-08-22

A field-report release: five things fixed or added, three of them from Randy
N4OPI.

**A stuck exchange that never logged (#234, Roy KI0ER, working K7FD).** His
diagnostic log showed `K7FD KI0ER R-17` retransmitted every slot for over ten
minutes while K7FD kept sending RRR, and the contact never logged. Root cause:
`ft8_qso_start()`'s only special-case for "this request already skips past
TX1" checked `kind == FT8_TX_KIND_REPLY` (a bare report reply). A
`FT8_TX_KIND_ROGER_RPT` request — built whenever the partner's own last
message to us was already a numeric report, so we reply with `R<report>`, not
a fresh grid — fell through to the unconditional default, `FT8_QSO_WAIT_RPT`.
That state expects THEIR FIRST report and has no "they already rogered us"
branch, only an explicit `got_rr73`/`got_73` skip-ahead, so every later decode
from that partner — including a bare RRR — was re-treated as a fresh report
and answered with another R-report, forever. `WAIT_RR73`'s handler already had
the correct bare-RRR handling (fixed for a different callsign, NH6L, back on
2026-07-27) and would have closed the exchange normally had the start state
been right. Fixed by adding a matching branch for `FT8_TX_KIND_ROGER_RPT` that
starts in `FT8_QSO_WAIT_RR73` instead — the single choke point all three ways
of starting this kind of exchange share (the auto-pileup drain, the pileup
modal, and a decode-row Transmit/Auto Pounce), so one fix covers all three.
Regression-tested in FT8 simulation mode: a full QSO through the exact
`WAIT_RPT`/`WAIT_RR73` transition this touches completed and logged normally.
Not yet reproduced against the specific 3-message sequence from Roy's report —
that needs a partner whose first message to us is already a report, which
sim's phantoms don't organically produce.

**The SWR-protection fault is visible from the web page now (Randy N4OPI).**
Before this the web UI had no visibility into a latched SWR trip at all — the
abort behind it read through as a bare "QSO Cancelled", and clearing it
required walking over to the Tab5 and tapping its prompt. `/api/status`'s
`ft8` block now reports `"swr_fault"` above every other TX/QSO state, same
wording as the Tab5's own left-pane label (`SWR X.X:1 - TX STOPPED - check
antenna - tap to clear`), and the banner is tappable in the browser exactly
like the Tab5 prompt — new `/api/cmd` action `clear_swr` calls the same
`ft8_tx_clear_swr_trip()` the Tab5 button does.

**"Who is hearing me" gets time windows and sortable columns.** The device
always answers with its fixed 24 h query, and every report carries its own
heard-at timestamp, so 15 min / 30 min / 6 h / 24 h chips and per-column
sorting are both pure client-side re-slices of that one fetch — no extra
device round trip per window or per column click. Summary figures (receiver
count, furthest distance) are recomputed per window rather than always
showing the full-24h totals. The FT8/FT4 decode list in the browser gets the
same sortable, clickable column headers, with a "↺ CQ callers on top" link
that appears once a column click has replaced the device's own
`ft8_screen_sort_rows` ordering, so nothing is lost getting back to it.

**Working an older pileup caller from the web page now actually works (Randy
N4OPI).** Clicking a "Calling you:" entry used to refuse outright —
`"%s is no longer in the decode list"` — the moment the caller's row had aged
out of the live decode table (`FT8_ROW_STALE_SEC`), even though the Tab5's own
pileup modal has always had a working fallback for exactly this case
(`ft8_pileup_modal.c`'s `row_work_cb`, added earlier for Roy KI0ER and Ken
KF0AYY field reports). `web_reply_drain()` (`ft8_screen_view.c`) now falls
back the same way: when the caller isn't in the live decode snapshot, look
them up in the pileup list instead and build a report-first reply from the
pileup entry's own cached SNR — the same shape `cqrun_answer()` and the Tab5's
pileup modal already use. Genuinely gone from both lists still refuses, which
is still correct (nothing to reply to).

Pileup entries now show their age too, on both screens: the web page's
"Calling you:" line gets `"Xs ago"`/`"Xm ago"` next to each caller (the
backend already sent `age` in `/api/decodes`' pileup array; only the frontend
was missing it — the Tab5's own pileup modal has shown this since it was
built). And the Tab5's main decode list's HRD (heard-count) column is now
**AGE**, in seconds since last heard — a more useful number for judging how
much to trust a row than how many times it's been seen, and matches what the
browser's decode list already showed.

Since that number now genuinely matters, **how long a row survives is
operator-tunable**: a new "Max age:" dropdown in the Filter modal (same row as
"Show only CQ callers"), 30/45/60/75/90 seconds, defaulting to 90 — the value
`FT8_ROW_STALE_SEC` has held since 2026-07-19. Stored as `ft8_filters_t`'s
appended `max_age_sec` byte; 0 (an NVS blob written before this field existed)
reads as "use the 90 s default," never as "expire instantly." The pileup list
is deliberately **not** covered by this setting — it has no expiry of its own
by design, which is the entire reason the report-first fallback above exists.

**Simulation mode no longer leaves real stations flickering on screen.**
Turning sim mode ON now clears the decode list and pileup immediately,
mirroring the existing clear on turning it OFF (previously only the OFF
transition did this). That alone wasn't enough with a real QMX still attached
and receiving: real decodes were never gated by `sim_mode_en` at all, so a
genuine station decoded moments after the entry-clear would silently
repopulate the list — reported as "some of the previous stations is
reappearing on and off... then finally disappear" (they were real, still
being decoded normally, and only stopped once they aged out or the band
moved on). Root cause confirmed by reading `decode_candidate_range()`
(`ft8_test.c`): its call to `ft8_screen_record_decode()` for a real decoded
candidate was unconditional, with no sim-mode check anywhere in the path —
unlike PSK Reporter spotting, which `net/pskreporter.c` already refuses while
sim mode is on, and unlike TX, which `ft8_tx.c`'s hard interlock already
blocks. Fixed by gating that one call on `sim_mode_en`, loaded once per
decode-candidate-range call (both decode-task cores) rather than per
candidate. Verified on hardware: with sim mode on and a real QMX attached and
streaming, all 7 phantom stations reached the decode list over multiple
cycles, each one paired with its own `ft8_sim: injected` log line, and zero
unlabeled (real) decodes appeared.

---

### Shipped in v1.9.3 — 2026-08-23

**The update flow stopped being a thing you had to learn.**

#### Updating, rewritten

Don N2VGU pointed out that "tap to update" described the wrong action at the
wrong moment: by the time it appeared the download was already on the device
and what remained was a restart. He was right about the cause rather than the
wording — the whole conversation lived in a ~264 px slot between the SD text
and the clock, so every state had to fit about twenty characters, and there was
no room to say what was actually happening.

- **The update now downloads quietly in the background** when one is available,
  so the only thing left for the operator is one decision. On by default, with
  **Settings → Network → Download updates automatically** to switch it off —
  each update is 3.3 MB, which is not free on the phone hotspot a POTA operator
  is using in a field. Downloading never applies anything; only a restart does,
  and only the operator can ask for that.
- **A window in the middle of the screen** (`ui/ota_modal.c`) carries the
  conversation in readable type with named buttons: *Restart now* / *Later*, or
  *Download now* / *Try again* / *Check now* depending on the state. It re-reads
  the live state every 500 ms, so a download that completes while it is open
  turns "Downloading" into "Restart now" by itself.
- **The bottom bar announces and no longer negotiates.** The line breathes
  gently while an update waits for a decision, and goes quiet once *Later* has
  been pressed — a pulse that never stops stops being a signal.
- **The 700 ms long-press is gone.** It existed only so that a stray brush from
  the 22 px band-plan strip directly above could not start a download or reboot
  a radio. Now that a press only opens a *dismissible window*, a stray brush
  costs nothing, so a plain tap is safe and the label saying "tap" is finally
  true. #237 dissolved rather than being reworded.
- **The band plan is much easier to hit.** It is 22 px with the bottom bar hard
  against it below and the waterfall hard above, all three doing different
  things with a tap. An invisible 50 px catcher above the strip gives it 72 px
  of touch while it still draws as 22; tap-to-tune gives up the same 50 px,
  which a 370 px waterfall can spare.

#### Internal memory headroom, which is what made background download possible

Background download is only safe if the device has room to do it while
everything else keeps running, and it did not. `ota_update_start()` needs 8 KB
of *contiguous internal* RAM for its task stack — it cannot use PSRAM, because
flash writes run with the cache off — and in FT8 with the radio streaming the
largest internal block measured **6,912 bytes**.

`size` → `size-components` → `nm --size-sort` named the two worst offenders in
four minutes: **`s_toc`** (9,472 B, the manual's contents list, touched only
while the Reader is open) and **`s_pub`** (4,608 B, a spur map for a feature
that defaults off), both sitting in internal `.bss` for no reason.
`EXT_RAM_BSS_ATTR` moved them, recovering **14,084 bytes**.

Measured in FT8 with the radio streaming ~48,000 pairs/s:

| | before | after |
|---|---|---|
| largest internal block | 6,912 | **19,456** |
| DMA pool free | 4,683 | **20,231** |
| minimum internal free | 2,519 | **10,067** |

The **OTA task's stack is now static and right-sized**: measured use is 3,216
bytes, so the 8,192 it asked the heap for was a round number rather than a
figure — and asking for it at the moment an operator presses "update" is asking
at the worst possible time. A static 6,144-byte `.bss` stack is 2 KB smaller
*and* cannot fail to allocate.

⚠ Engineering detail, including the falsified theories and the controlled A/B
that settled it, is in `CLAUDE.md` rather than here.

#### A real bug in every upload, not just updates

`fft_task`'s transfer-quiet branch read **one window (1,024 pairs) then slept
50 ms** — about 20,000 pairs/s against the ~48,000 the QMX produces. It fell
behind more than 2:1 and overflowed the audio ring for the whole of *every*
upload and every download, while its own comment claimed it prevented exactly
that. Measured as `DROPPED=17904 (ring full)` then `DROPPED=28128` a second
later. It now drains until the backlog is gone. This has been wrong since the
flag was written and affects QRZ, eQSL and LoTW uploads too.

Related: the download loop no longer stands the FFT down at all — the spectrum,
waterfall and FT8 decoding keep running throughout, with the quiet period
reduced to the ~1.9 s image verify at the end.

#### Also in this release

- **TXCQ ANY / EVEN / ODD on the web FT8 page** *(Randy N4OPI)* — the browser
  could start a CQ but not choose which 15-second window it went out in. It
  drives the same single piece of state as the Tab5's own button and re-renders
  from the device, so the two surfaces cannot drift apart.
- **SSB tune snap 250 → 500 Hz** *(Dave KX3DX)* — he asked for 1 kHz, noting
  that stations which stray sit at 0.5 kHz; 500 Hz was chosen because a 1 kHz
  grid cannot land on the very exception he named, and it halves the number of
  stops across a drag.
- The bottom-bar version line no longer collides with the SD text and the
  clock: `short_ver()` existed for exactly that and had never been applied to
  the *running* version, so a development build rendered its full
  `-N-gHASH-dirty` string in a shrunken font. Shortened in the composite update
  lines only — the idle bar still shows the full string, which is deliberate.

#### Known limitation

The arrow between the two versions is plain ASCII `->`. LVGL's symbol-font
arrow is a chevron that reads as `>` and is too heavy at 24 px; U+2192 is not
in this font and renders as a tofu box. A real arrow needs `montserrat_24`
regenerated with `lv_font_conv` — logged as #244.

### Shipped in v1.9.4 — 2026-08-25 20:46 UTC

A feedback-batch release: the FT8-specific settings get their own home on
both screens, the TX tone picker stops going stale while you decide, and the
web page finally works on a phone held upright.

**FT8 Options — one place for everything that's only meaningful in FT8
mode.** The Tab5's left-pane button was renamed **Filter → Options** (Roy
KI0ER): the modal it opens already held real behaviour toggles — Auto-work
pileup, grey-listing — not just filters, so the old name undersold it. The
web UI gets the equivalent concept properly for the first time
(`main/net/www/index.html`): CQ message presets and the FT8 filters group,
previously scattered through one long general Settings list behind the
bottom bar, now live behind a dedicated **Options** button next to **TX
tone**, rendered only in FT8 mode (`FT8_OPTS_GROUPS`, `setBuildForm(cfg,
ft8Opts)`). The entry button carries a **count** of currently-active
settings rather than a binary colour — a colour would always read "active"
for an operator who runs filters permanently, telling them nothing new each
time (Dirk DK7CVD's "a more granular approach" refined against Roy's
objection to a plain on/off indicator) — and hovering it lists which ones by
name (`ft8OptsSummarize`). Inside the panel, an active checkbox gets an
amber border/fill in addition to its own tick mark (Don N2VGU's
accessibility point: colour alone is unreadable to colour-blind users, so
the checkbox's own shape carries the signal too, the colour is redundant on
top of it, never the only channel).

**The TX tone picker no longer goes stale while you're deciding.**
`toneOpen()` fetched `/api/tone` exactly once, on open, with no refresh
until Apply or Cancel — so a slow decision (reading the E/O strip, weighing
which window to use) could cross a 15-second FT8 boundary for free, and a
station that landed on the slot you were about to pick stayed invisible the
whole time. `toneRefresh()` now re-polls every 3 s while the modal is open,
via `setInterval`/`clearInterval` on `toneOpen()`/`toneClose()`; the
operator's own in-progress pick (`toneSel`/`toneHold`) is never touched by a
refresh, only the busy/partner colouring redraws under it.

**The web page works on a phone held upright.** `html, body { overflow:
hidden }` combined with a `@media (max-width: 600px)` rule that outright set
`#top-right { display: none }` meant a narrow portrait screen didn't clip
overflowing controls, it deleted them from layout with no way back — landscape
never hit this because there was room for the same content at its natural
size (Randy N4OPI, iPhone Safari). Fixed in two parts: `#top`/`#bot` now
scroll horizontally (`overflow-x: auto`) instead of clipping, the same
pattern the decode-list table already used, and the narrow-width media query
no longer hides `#top-right` at all — swiping reaches it instead. A
`@media (orientation: portrait)` rule also allows the page itself to scroll
vertically as a fallback for the rare case the fixed-viewport grid still
doesn't fit.

**Battery display simplified, and now says when charging is capped on
purpose.** Cell voltage (`main/util/status.c`'s `%d.%dV`, and the matching
`.mv` render in `index.html`) is dropped from both the Tab5 and the web
page's battery readout — the percentage already carries the level, and
nobody reading the bar needs the raw voltage underneath it. In its place:
once the Battery Care charge limit trips (`s_charge_cutoff_active`), the
reading now appends `(limit)` on **both** screens — a new
`status_charge_limit_active()` accessor feeds `/api/status`'s `battery.limit`
field so the web page can show the same thing. Before this, "capped on
purpose at 80%" and "not charging for some other reason" read identically
(Don N2VGU).

**The snap-on keyboard can be attached — or reattached — any time, not just
at boot.** `tab5_keyboard_init()` used to probe once at startup and, on
failure, tear the I2C bus down entirely; a keyboard snapped on after boot
was never found for the rest of the session. Replaced with a single
persistent lifecycle task (`kb_task`) that keeps retrying every 2 s while
absent and detects a genuine detach (5 consecutive failed polls, ~250 ms —
long enough to ride out a transient bus glitch, short enough to notice a
real unplug quickly) by watching for the STM32 to stop acking, so unplug and
reattach mid-session both keep typing working without a Tab5 reboot —
verified live on hardware, caught mid-cycle in the serial log (`no longer
answering - treating as detached` at 27.1 s, `Tab5 keyboard detected` at
33.2 s). The keyboard's two RGB LEDs are forced dark (`REG_RGB_MODE` =
custom, both channels zero) on every claim, including a reattach — an
earlier attempt at this release tried a battery-status readout on the LEDs,
then a plain "active" green, and both were reverted: the LEDs run far
brighter than any status glyph needs, the Tab5 screen already shows battery
state, and stock "binding" mode turned out to be a richer state machine than
assumed (purple on a hot attach, green on a boot attach, contradicting the
simple attach/active pair this project's own notes originally described) —
so once the keyboard is claimed, there is nothing left for a light to
usefully distinguish, and it says nothing rather than something misleading.

**The macOS/Linux flasher script works on more systems.** `flash.command`
could ship with Windows line endings despite `.gitattributes` declaring
`eol=lf` for it — a working-tree copy had drifted to CRLF, which some
shells refuse outright (`bad interpreter: ...^M`) — caught and diagnosed by
a user on Fedora rather than by us (Michael K Johnson KZ4LY).
`tools/make_flasher_zip.ps1` now strips CR from every `.command` file
unconditionally at packaging time, regardless of what state the working
tree happens to be in on whatever machine builds the release.

#### Also in this release

- **Line-ending policy fixed repo-wide.** The same class of drift that hit
  `flash.command` turned out to affect 84+ other tracked `.c`/`.h`/`.html`
  files (LF-committed, CRLF on disk) with nothing in `.gitattributes`
  preventing it from recurring. Added `eol=lf` for `.c/.h/.html/.md/.py/
  .json/.yml`, matching the existing `.command`/`.sh` rule, and normalized
  every currently-drifted tracked file (`git add --renormalize .` found 59
  more beyond the initial extension-based sweep, mostly `test/wav_reference/
  *.txt` fixtures) — every file individually byte-verified as EOL-only
  before being included, zero functional change, rebuilt and binary-size-
  matched twice to confirm. Developer-only; no user-visible effect.

### Shipped in v1.9.5 — 2026-08-25 23:20 UTC

A fast-follow patch: two ADIF logging bugs, both able to cost real credit or
real data. No UI or feature changes.

**POTA/SOTA activation count double-counted duplicate contacts (Eric,
GitHub issue).** `adif_log_count_activation()` counted every logged record
toward POTA's 10-QSO (SOTA's 4-QSO) minimum, with no dedup by callsign - it's
the one function both the Tab5's Activation modal and the web UI's
activation pill read from, so both screens showed the inflated number.
Working the same station twice (Eric: KO4JON) made the device say "10
contacts, park activated" while POTA.app credited 9 unique stations and
rejected the activation - happened on three parks in one outing before he
worked around it by ignoring the on-device count entirely. Fixed by
deduping on callsign alone, not band/mode: the conservative direction, since
it can only ever show a number at or below what POTA would actually credit,
never claim activation early the way the old count could.

**Single-record delete could silently fail, or corrupt the log on a card
with damaged storage.** `adif_log_delete_record()`'s rewrite-to-temp-and-
rename never checked whether the write actually succeeded before deleting
the original file and renaming the (possibly incomplete) temp file over it
- unlike the QSO-append path in the same file, which already has this exact
`ferror`/`fflush`/`fsync`-checked pattern. Reported as "the delete UI runs
through its whole motion but the record is still there afterwards."
Root-caused on the dev bench down to the byte: `fopen("/spiffs/qso.tmp",
"w")` returning `ENOSPC` (errno 28), on a partition reporting 701 KB used of
934 KB while its actual files (`qso.adi` + `diag.0.log` + LoTW cert/key)
only added up to ~170 KB - the ~530 KB gap, plus a directory entry that
couldn't even be `stat()`'d, was orphaned/inconsistent SPIFFS index blocks,
not legitimate usage. `esp_spiffs_check()` (ESP-IDF's own consistency
check/repair) reported success but reclaimed nothing; `esp_spiffs_gc()`
then confirmed why (`SPIFFS_gc failed`, 0 bytes reclaimed) - that bench's
partition had a genuinely unrecoverable page, proved by a full format +
hardware self-test (write two records, delete one, verify the count: PASS).

Fixed two ways: `adif_log_delete_record()` now verifies the rewrite
actually succeeded before touching the original file, and surfaces a toast
instead of failing silently either way - so a real write failure is never
mistaken for "it worked." And since `esp_spiffs_check()` + `esp_spiffs_gc()`
together are far cheaper than asking an operator to reboot, both now run
once at mount (`adif_log_init()`), and the delete path retries once through
the same repair if it still hits `ENOSPC` live - so a real field unit whose
diag log has fragmented storage over a long uptime self-heals instead of
needing a power cycle, which is the case this bench's *un*recoverable page
was standing in for.

⚠ The dev bench's own corruption was NOT repairable in the end - it took a
full SPIFFS format (which wipes the partition: the ADIF log, the diag log,
and the LoTW certificate/private key) to recover the bench itself. That is
a one-time, hardware-specific recovery, not something this release does to
anyone's device automatically or ships as a user-facing action.

### Shipped in v1.9.6 — 2026-08-26

A POTA/SOTA logging release, all of it from a seven-page report Don Adams
WB0LQW wrote after four real park activations, comparing what the Tab5 writes
against what POTA, SOTA and the ADIF specification actually ask for.

**Your callsign is now `STATION_CALLSIGN`, the field POTA reads.** We wrote it
as `MY_CALL`, which POTA accepts and then warns about on every single upload:
*"No station_callsign field, assuming operator WB0LQW"*. It was guessing - it
happened to guess right, from his account, but a log should say who made the
contact rather than leave it to be inferred. `STATION_CALLSIGN` is the
spec-correct field and is what the log now writes. Records logged before this
release keep the old field name; POTA still accepts them, with the warning.

**Park-to-Park and Summit-to-Summit contacts can be entered afterwards.** The
web log editor (**QSO Logs -> View / edit log**) has a new **P2P ref** column.
This is the one piece of a park-to-park contact that no radio can tell you:
while you are operating, the other activator's park number is on the POTA spots
page on your phone, and nothing in the FT8 exchange carries it. So you note it
down, and when you get home you click that cell and type the reference -
`US-1241`, `G/LD-049`, `DLFF-0123`. The Tab5 works out the programme from the
reference's shape and writes both `SIG` and `SIG_INFO`, which is what POTA reads
to award the P2P; clearing the reference clears both again. A chase that was
logged from a spot already carried these fields automatically - this is for the
contacts made without one.

A reference must contain a dash to be accepted. `US1241` is refused rather than
written, because that value goes out as a claim that a specific park was worked,
and a missing reference is honest where a wrong one is not.

**FT4 contacts are logged the way the ADIF specification defines them:**
`MODE=MFSK` with `SUBMODE=FT4`, instead of `MODE=FT4`. ADIF makes FT8 a mode in
its own right but FT4 only a submode of MFSK - an asymmetry, but the standard
is the standard, and it is what WSJT-X writes and what other software expects to
read. POTA accepted our old form, but ADIFMaster - the free editor Don uses to
prepare a log before submitting it - refuses to open a file that declares FT4 as
a mode at all. FT8 records are unchanged.

LoTW uploads are unaffected: LoTW keeps its own list of modes and has no MFSK in
it, so the pair is turned back into `FT4` before a QSO is signed, and the file
that goes to LoTW is byte-for-byte what it was before. eQSL and QRZ both prefer
the new form. As with the callsign field, FT4 QSOs logged before this release
keep `MODE=FT4`; a **Today** download after upgrading gives you a file in the
new form.

**Also fixed:** editing a record verifies the rewrite before replacing the log,
the same protection single-record delete got in v1.9.5 - on a filesystem that
has filled up, an edit now fails cleanly and says so instead of risking the log
it was rewriting.

Two of Don's suggestions were deliberately not implemented. Dropping
`FREQ`/`RST_SENT`/`RST_RCVD`/`GRIDSQUARE`/`MY_GRIDSQUARE` would tidy the file
for POTA, which ignores them, at the cost of QRZ, eQSL, LoTW and your own record,
which do not. And SOTA's own `MY_SOTA_REF`/`SOTA_REF` fields are still not
written - `SIG`/`SIG_INFO` is valid ADIF for a summit too, but whether SOTA's
uploader reads it needs a SOTA activator to confirm before anything is changed.

Alongside the ADIF work, this release carries the first reports off v1.9.4 and
v1.9.5.

**The bottom-bar menus work again on an iPhone.** Tapping *QSO Logs* highlighted
the button and did nothing at all *(Travis AK6TB)*, or opened the menu behind the
decode list so a selection could be made but not read *(Randy N4OPI)*. Both come
from one line added in v1.9.4 to make the bars scroll sideways in portrait: it
also makes Safari treat the bar as the frame those popups are positioned inside,
which either clips them away or drops them behind the page. That line is gone,
and the menus have been moved out of the bar entirely so no future change to the
bars can reach them.

**"Check for updates" no longer says you are up to date when you are not.** A
check asks GitHub and takes a few seconds; both the Tab5's update window and the
browser's version label were repainting the *previous* answer immediately after
the press, so you were told "Up to date, you are running v1.9.3" about a release
that already existed — then the offer appeared on its own a moment later, by
which point it read as the button having failed *(Michael KZ4LY, Samuel W7STF)*.
Both now say **checking** until the answer actually arrives.

**The mouse wheel tunes the radio.** Over the spectrum or the waterfall, one
click is **10 Hz in CW and the digital modes** — fine enough to zero-beat a CW
signal by ear — and **100 Hz in SSB** *(Roy KI0ER, seconded by John Dusek)*. It
stops at the band edges, a fast spin is not lost, and anything covering the
panadapter takes the wheel instead, so the dial never moves under a window you
are reading.

**The wheel also stops scrolling panels into blank space.** Winding past the last
row of the settings list or the QSO log used to carry the contents off the screen
entirely, leaving the panel empty until you wound it back *(Roy KI0ER)*. It now
stops at the ends, and hands the click to whatever is behind when there is
nothing left to move.

**You come back from the radio's own menus where you left.** Using **Radio
menus** to check something — Hardware Tests → Diagnostics, say — could leave the
radio on 160 m whatever band you started on *(Randy N4OPI)*. Closing the session
now puts the frequency and mode back if they moved, alongside the I/Q-mode
re-enable that was already there. If you changed band deliberately while you were
in there, that gets put back as well: the Tab5 cannot tell the two apart.

The rest of this release came from the first days of v1.9.4 and v1.9.5 in the
field.

**The mouse wheel tunes the radio.** Over the spectrum or the waterfall, one
click is **10 Hz in CW and the digital modes** - fine enough to zero-beat a CW
signal by ear - and **100 Hz in SSB** *(Roy KI0ER, seconded by John Dusek)*. It
stops at the band edges, a fast spin is not lost, and anything covering the
panadapter takes the wheel instead, so the dial never moves under a window you
are reading.

**The wheel also stops scrolling panels into blank space.** Winding past the last
row of the settings list or the QSO log used to carry the contents off the screen
entirely, leaving the panel empty until you wound it back *(Roy KI0ER)*.

**The bottom-bar menus work on an iPhone again.** Tapping *QSO Logs* highlighted
the button and did nothing at all *(Travis AK6TB)*, or opened the menu behind the
decode list so a selection could be made but not read *(Randy N4OPI)*. Both come
from one line added in v1.9.4 to make the bars scroll sideways in portrait: it
also makes Safari treat the bar as the frame those popups are positioned inside.
That line is gone, and the menus have been moved out of the bars entirely so no
future change to the bars can reach them. Travis confirmed the diagnosis from the
other end - broken in Safari on macOS, iOS and iPadOS; fine in Edge and Chrome.

**"Check for updates" no longer says you are up to date when you are not.** A
check asks GitHub and takes a few seconds; both the Tab5's update window and the
browser's version label were repainting the *previous* answer immediately after
the press *(Michael KZ4LY, Samuel W7STF)*. Both now say **checking** until the
answer actually arrives.

**Background downloading is a switch.** It always was a setting, but it had no
control anywhere - the config file was the only way to reach it, which is not an
opt-out in any useful sense. It is now a checkbox in the Tab5's Settings under
**Network**, and a row in the web Settings under **Updates**. Turn it off and the
Tab5 still checks and still tells you a new version exists; it simply waits for
you to ask before spending 3.3 MB, which matters on a phone hotspot *(Michael
KZ4LY, Samuel W7STF, Steve N9SZ)*. Applying an update is a deliberate press
either way - nothing installs itself.

An automatic download now also waits until the Tab5 has been up for five minutes
and has memory to spare. Steve N9SZ saw one start moments after boot, appear to
finish, and then reboot the device back onto the old version - twice - while the
same update started by hand worked. The download is verified before it is made
bootable, so a reset during that step loses the download rather than anything
else, but starting 30 seconds into boot is the worst moment on this hardware.

**You come back from the radio's own menus where you left.** Using **Radio
menus** to check something could leave the radio on 160 m whatever band you
started on *(Randy N4OPI)*. Closing the session now puts the frequency and mode
back if they moved. If you changed band deliberately while you were in there,
that is put back too - the Tab5 cannot tell the two apart.

**Basic/Expert is remembered** across a reboot *(Samuel W7STF)*, and switching
FT8/FT4 from the web API lands on that mode's calling frequency instead of
staying on the other one's.

**Under the bonnet**, the built-in station simulator can now run FT4 - it had
three separate places that assumed FT8's 15-second slots - and its practice
stations now have signal levels that vary instead of every one reading the same.
That is bench equipment rather than a feature, but it is how FT4 and
signal-report behaviour get tested without putting a real station on the air.

### Shipped in v1.10.0 — 2026-08-28

**WSPR.** The Tab5 gains a third page, alongside the panadapter and FT8/FT4.
WSPR is a propagation beacon rather than a contact mode: you transmit a very
slow, very weak signal carrying only your callsign, grid and power, and stations
worldwide report hearing it. Over an evening you get a picture of where your
antenna and your band actually reach, at power levels where nothing else would
be heard at all. Nobody replies, and nothing goes in your log.

Swipe → now cycles **Panadapter → FT8/FT4 → WSPR**. The page shows the stations
heard in each two-minute cycle with distance and bearing, the furthest of the
session, a per-cycle history so an opening band looks different from a closing
one, and the captured 200 Hz window. Receiving is the default and is worth doing
on its own; transmitting is opt-in and, like FT8, refuses to key without your
callsign and grid.

**Its settings live in the drawer**, under WSPR, and appear only on that page:
allow transmitting, declared power, duty cycle, band hopping and whether to
publish what you hear to wsprnet.org. Duty cycle and band hopping used to be
buttons on the page itself; both are decisions made once for a session rather
than controls reached while watching spots arrive, so the page keeps only the TX
switch. Ticking two or more bands in the picker is what turns hopping on.

**Declared power is a claim, not a measurement** — the Tab5 cannot know what
your radio delivers, and every spot publishes that number worldwide into a
database other operators reason from. Set it to what your transmitter really
produces.

**The Tab5 now wakes up on the page you left it on**, including after a firmware
update. If that page is WSPR with transmitting enabled, the station resumes
beaconing on power-up — which is what a beacon is for, but worth knowing before
you leave the shack.

**Basic and Advanced.** The settings drawer's EXPERT button is now **ADVANCED**,
which names the contents rather than the reader. More usefully, which settings
appear in which view is no longer fixed: the web UI's Settings window has a
**Tab5 config** button opening a table of every setting with Basic and Advanced
ticks. Basic now holds the eight things an operating session actually reaches;
everything remains in Advanced. A firmware update that adds a setting will show
it rather than hiding it behind a layout saved before it existed.

**Two field reports from Gyula HA3HZ**, both fixed. A station you had just
worked could be called again within minutes — the decode list greyed the
callsign while the engine ignored it unless a filter was ticked, so the screen
and the machine disagreed. The automatic pickers now leave a station alone for
30 minutes after working it, whatever the filter says. And the red **FREQ BUSY**
warning had no signal-strength test at all, so a barely-audible station on the
other side of the world raised the same alarm as a loud neighbour; it is now
graded by strength, and hidden entirely during an exchange, where your transmit
tone is deliberately locked to your partner and moving is the wrong thing to do.

**The printable User Guide gains two chapters** — WSPR, and **Radio menus**, which
had a line in the contents but no chapter behind it since v1.8.4. The PDF builder
only injects a chapter where the README carries a matching heading, and neither had
one; the guide is 116 pages now rather than 107.

**Also:** the Tab5's frame rate and redraw load are now in the diagnostic log,
Bluetooth's host task moved off the display core, several task stacks were
returned to the pool, and a WSPR session no longer leaves the FT8 page tuned to
the WSPR frequency.

### Shipped in v1.10.1 — 2026-08-29

**WSPR now protects your radio's finals, and the declared power stops being a
guess.**

A WSPR transmission keys the radio for about **110 seconds out of every 120**.
Nothing else the Tab5 does comes close — an FT8 burst is about 12 seconds — and
running a QMX flat out on that cycle puts sustained heat through the PA
transistors. QRP Labs warn about exactly this case in the QMX manual, and the
radio's own built-in WSPR beacon turns its PA down for the same reason. The
Tab5's WSPR transmit is driven over CAT and never enters that mode, so it
inherited none of that protection.

**Protect finals**, on by default, turns the radio down for as long as WSPR
transmit is enabled — it sets the QMX's own *Max. PA voltage* to about 6 V and
restores your setting afterwards. Measured on a QMX at 12 V: output falls from
**5.4 W to 1.6 W**, and the heat in the PA transistors falls **76%**.

It is a full-width button that states which state it is in — green *"ON - about
1 W"*, or red *"OFF - FULL POWER, finals at risk"*. Turning protection off takes
two deliberate taps; turning it back on takes one. While it is off, the TX block
on the WSPR page reads **FULL PWR** in red, so a station cannot be left running
unprotected without it being visible.

⚠ **Protection reduces the heat in the finals; it does not remove it from the
radio.** The QMX limits PA voltage with a pass transistor, so the difference is
dropped inside the radio instead — total heat fell only 18% in the same
measurements. If you intend to beacon for hours, **feed the QMX from a lower
supply**: it accepts 6.0–12.0 V, and around 9 V leaves far less to throw away.
That is the one thing no firmware setting can do for you, and it is now in the
manual.

**Declared power is advised by measurement.** During each transmission the Tab5
asks the radio what it is actually producing and shows the answer under the
setting — *"radio measured 1.6 W last burst = 32 dBm"*. Switching protection on
or off also moves the declared figure to the value that setting normally gives.
Both are suggestions; the number is a statement about your station and stays
yours to choose. The list now runs to **37 dBm (5 W)** again: a declared power
never commanded the radio, so limiting it could only have prevented an honest
declaration.

**Also fixed:** the checkboxes in the FT8 **Options** window were very hard to
hit *(Don WB0LQW)* — the touch target was the small box alone, a tap that
drifted a few pixels was swallowed by the panel behind it, and the word beside
each box did nothing. The touch area is now much larger, the tap cannot be
stolen, and **tapping the word toggles the setting**. The WSPR settings are also
reachable from the browser for the first time, and the web and Tab5 lists agree.

### Shipped in v1.10.2 — 2026-08-30

**Bluetooth keyboards work, and two logging faults are fixed.**

- **A Bluetooth keyboard now types into every field** *(Don N2VGU)*. Pair one and it works everywhere the snap-on keyboard already did: every text field, Enter and Esc in every window, Tab to move between fields, and the arrow keys. The on-screen keyboard steps aside while a Bluetooth keyboard is connected, which is the point of having one - it gives you back the screen space it was covering.
- **A keyboard and a mouse can be connected at the same time.** Either one first, the other after, and both keep working. If your keyboard sleeps, it reconnects on the first keypress and anything you typed while it was waking is delivered rather than lost.
- ⚠ **US keyboard layout only for now.** A Bluetooth keyboard reports key *positions* rather than characters, so letters, digits, Enter and the arrows are correct on any keyboard, but punctuation on a non-US layout will not be. National layouts are coming.
- **A logged contact could be missing the received signal report** *(Gyula HA3HZ)*. If the other station's message already carried their report of you, the Tab5 replied and moved straight to the roger step - and their report, which was sitting in that very message, was never written to the log. It is recorded now. Contacts already in your log are unaffected; this applies to new ones.
- **FT4 replies no longer go out a cycle late** *(Gyula HA3HZ)*. FT8 waits for the current slot to finish decoding before it transmits, so a fresh reply lands in the right slot. That had never been switched on for FT4, which is why FT4 exchanges took roughly twice as long as they should. Now on for both.
- **The RIT indicator says what it is doing** *(Don N2VGU)*. With no offset engaged it read simply "RIT", which is easy to mistake for switched on. It now reads **RIT OFF** in grey with a line through it, so all four states - off, armed, engaged, and parked - state what they are rather than naming the feature.
- **Declared WSPR power set from the browser now shows on the Tab5.** The Tab5's own dropdown kept the value it was built with, so the two screens could disagree about a figure that is published with every spot.

### Shipped in v1.10.3 — 2026-08-30

**FT4 transmits again — update if you use FT4.**

- **FT4 would not transmit at all** *(Gyula HA3HZ)*. In v1.10.2 an FT4 transmission was held back at the start of its slot and then never sent: the countdown ran normally, the message was armed, and nothing went on the air — on a CQ and on a call alike. FT8 was unaffected throughout. Fixed, and FT4 now transmits at the start of its slot as it did in v1.10.1.
- **The FT4 reply timing from v1.10.2 is withdrawn with it.** That release made FT4 wait for the current slot to finish decoding before transmitting, so a reply landed in the right slot instead of a cycle later — and that is exactly the change that broke transmitting. FT4 replies can again be a cycle late. It needs a transmit window sized for FT4's shorter slot, which is being done properly rather than quickly.

### Shipped in v1.10.4 — 2026-08-30

**FT4 replies are quick again, and the Tab5 stops inventing an FT4 power reading.**

- **FT4 answers in the right slot** *(Gyula HA3HZ)*. FT4 now waits for the current slot to finish decoding before it transmits, the same rule FT8 has used since v0.21.0, so a reply goes out in the slot it belongs to instead of a cycle later. It should feel at least as quick as FT8. This was tried in v1.10.2, broke FT4 transmit entirely, and was withdrawn in v1.10.3 — it is back, this time with the transmit timing checked by an automatic test rather than by eye.
- **FT4 no longer reports a made-up 5.0 W** *(Gyula HA3HZ)*. An FT4 symbol is 48 ms and asking the radio for its power can take 50 ms, so the power cannot be sampled during an FT4 transmission the way it is in FT8 — and the display was filling that gap with a fixed 5.0 W. The radio's own reading was the correct one. **FT4 now shows no power rather than a wrong one**; FT8 is unaffected and still measures properly.
- **Auto-work pileup leaves busy stations alone** *(Gyula HA3HZ)*. A station who had called you could be answered a couple of minutes later, by which time they were already in a contact with somebody else — and then called repeatedly. If their last message shows them working another station, they are now skipped until they are free.

### Shipped in v1.10.5 — 2026-09-01

**The spectrum holds still while you tune across it, and a quarter of the ×1 view stops lying about where signals are.**

- **A still spectrum and waterfall.** The panadapter now behaves the way a Flex does: the spectrum and waterfall stay where they are and the VFO marker moves over them, so a signal stays put on screen while you tune towards it. It also makes the waterfall readable as history — a signal's past sits directly above its present, under the frequency it belongs to, instead of the whole picture sliding sideways every time you touch the dial. The view re-frames only when you tune far enough to need it: a dead band where nothing moves, a small push so a station sitting at the screen edge can still be worked, then a page carrying part of the old screen across so you can see where you came from. What triggers it is **your filter passband reaching the edge of the screen** rather than a fixed percentage of the view — the passband is not centred on the dial, so a percentage rule re-frames too early at one edge and too late at the other, and mirrors itself in LSB. On by default; switch it off under **Settings → Radio & display → Still spectrum**.
- ⚠ **At ×1 the display stays centred on the dial**, whatever that setting says. Holding a view still needs somewhere for it to stay while the capture window slides underneath, and at ×1 the view is already the whole 48 kHz the radio sends. Zoom to ×2 or beyond for the still display.
- **The right-hand quarter of the ×1 view was showing real signals at the wrong frequency.** The QMX's local oscillator sits 12 kHz below the dial, so the 48 kHz it delivers covers dial−36 kHz to dial+12 kHz and there is nothing at all above dial+12. The display filled that quarter by wrapping the bottom of the band into it, and the frequency scale labelled it as dial+12 to +24 — so the signals shown there were real, but about 48 kHz from where the scale claimed, and **tapping one tuned you to the wrong place entirely**. That region is now hatched and inert on the Tab5 and in the browser, with a caption saying why it is empty.
- **Leaving FT8 or FT4 could reboot the device a few seconds later.** Switching back to the panadapter tore down the decoder while one of its two decode tasks was still working on the last slot, and the memory it was reading was freed underneath it. The window is easy to hit precisely because you are going back to the panadapter: that puts the display work back on the same processor core the decoder shares, so the decode finishes more slowly at exactly the moment the teardown is waiting for it. The decoder now abandons the final slot's work as soon as you leave — those decodes were about to be discarded anyway — and the teardown refuses to free anything it cannot prove is finished with. **This bug is older than v1.10.5**; it was found while testing this release.
- **Changing band ends a contact in progress** *(Randy N4OPI)*. Switching bands used to stop the automatic picker choosing a new station but leave any exchange already running untouched — so it kept sending its next message on the new band, to a station no longer there. Changing band now ends the contact as well.
- **The WSPR transmit button has moved clear of the left edge** *(Randy N4OPI)*. It overlapped the edge-swipe strip used to reach the panadapter, so a swipe that started a little high could land on a control that keys the radio for about 110 seconds.
- **WSPR spots show the band they were heard on** *(Roy KI0ER)*, in a new **M** column. With band hopping on, a list of spots from several bands could not be read otherwise.
- **The WSPR waterfall marks a transmit cycle** *(Dirk)*. On a cycle you transmit, the receiver is stood down for the whole two minutes and there is nothing to draw — so the previous cycle's picture used to sit there looking frozen. The display now lays down its cycle-boundary marker and keeps the last received image below it.
- **The browser gets the FT8/FT4 band preset list** *(Randy N4OPI)* that the Tab5 has always had.
- **A LoTW upload now shows LoTW's own reply** *(Randy N4OPI)*. The count reported before was our count of what was *sent* — LoTW accepts a file and processes the contacts afterwards — so an upload could report success while nothing appeared in the log. The server's own message is now shown, which is what says whether anything was actually rejected.
- **The LoTW certificate can be replaced from a visible button.** Re-importing it used to need a Ctrl-click nobody would guess at, and a certificate expires about every three years.
- **A static IP address** can be set under **Settings → WiFi** — address, mask, gateway and DNS. Leave the address empty for DHCP, which is what every unit does today, so nothing changes unless you fill it in. ⚠ Get the subnet right: an address that is valid but on the wrong subnet leaves the Tab5 unreachable, and the web page is the only place to change the setting back.

### Shipped in v1.10.6 — 2026-09-01

**A release of things users found. Almost every item below came from a report on the air.**

**WSPR**

- **The waterfall no longer goes blank for two cycles around a transmission** *(Dirk DK7CVD)*. Only the transmitting cycle itself is dark now, which is the one that cannot be helped. The transmitter was being armed as much as two minutes before its slot, and the receiver stood down for that entire wait as well as for the transmission — so on a 50% duty cycle the band was going unheard about half the time it appeared to be listening. It now keeps receiving until the burst is actually due.
- **The TX button stops a transmission immediately** *(Roy KI0ER)*. Tapping it mid-burst used to do nothing visible until the end of the two-minute cycle. It now keys down at once.
- **Leaving WSPR gives the radio its power back** *(Roy KI0ER)*. With "Protect finals" on, WSPR reduces the QMX's maximum PA voltage to 6 V for the duration. That reduction was being left behind when you switched to CW or FT8, so those modes ran at a quarter power with nothing on the Tab5 to say so — it was visible only on the radio's own Protection menu. Leaving WSPR now stops any transmission first, waits for the radio to unkey, and only then restores the voltage.
- **The spot list fits the screen.** The right-hand column was running off the edge, so bearing was invisible and distance partly so. Every heading now sits directly over its own column, **M** is headed **BAND**, and a country name too long for its column is shortened rather than replaced by a two-letter code.

**Panadapter**

- **Picking a spot brings it into view** *(Roy KI0ER)*. With the still display on, a spot could land right at the screen edge — reliably, because the view only re-frames when something reaches an edge. Choosing a spot or a callsign now re-frames on it.
- **The still display holds completely still, then jumps** *(Dirk DK7CVD)*. It used to hold, then be dragged along by the tuning for a while, then jump — so the empty area at the edge kept changing size. The dragging is gone. At most half your filter width slides off the edge before the display re-frames.

**Network**

- **A static IP address that would lock you out is refused.** The Tab5 compares what you type against the network it is on and rejects an address on a different subnet, naming both. A blank mask, gateway or DNS is filled in from the current DHCP lease instead of assuming the usual home-network defaults — which is where this most often went subtly wrong. Setting one up in advance for a different network still works; it asks you to confirm.
- **A "Use DHCP" button on the Tab5** *(Michael KZ4LY)*. It appears in the settings drawer only when a static address is configured, and clears it. Until now the only way back was the web page that a wrong address had just made unreachable.

**Reliability**

- **A QMX restart could reboot the Tab5.** A power cycle while audio was streaming could catch the USB driver mid-teardown, and it stopped the device rather than carrying on. Worth knowing if you have seen this: the reboot also disturbs the radio, so what looks like the QMX wedging can be the Tab5 restarting underneath it.
- **The microSD card is handled better in three ways.** Its first write no longer collides with WiFi starting up; a failed background write no longer unmounts a card that is working; and the card is retried for an hour after start-up instead of being given up on. That last one also means **a card inserted while the Tab5 is running is now picked up**, within about five minutes, instead of being ignored until the next restart.

**Smaller things**

- A warning in the diagnostic log about the FT8 decoder "respawning" was describing entirely normal behaviour and made ordinary mode switches look like faults. It no longer does.
- The diagnostic log now says so if the device ever runs out of network connections — a state in which the web page stops answering while everything else keeps working.

### Shipped in v1.10.7 — 2026-09-02

**A release of things users found, plus one crash that had been in the firmware for months.**

**Reliability**

- **A crash that rebooted the Tab5 during FT8 is fixed, and it was our own leftover debugging code.** A diagnostic routine from earlier weak-signal development was still in the decoder, trying to open a log file **once per decoded candidate — up to 140 times a slot, every slot**. The file could never be created, so it tried again forever, and each attempt asked for a small piece of memory the device was short of. Eventually one attempt failed and the firmware stopped. It showed up as a cyan screen and a restart, usually a minute or two into an FT8 session. Two occurrences were captured on the bench and traced exactly before the fix.
- **Static IP addressing works.** It did not in v1.10.6 — the address you entered was always rejected and the Tab5 fell back to DHCP. Anyone who tried it saw it silently not take effect.
- **A timed-out contact no longer blocks the station.** When a call went unanswered the message stayed on screen until you tapped it, and while it was there the automatic answering would not start anything new. It now clears itself after 20 seconds, counting down so you can see it go, and tapping still clears it at once. On the web page it is a button you can click.

**WSPR**

- **Transmit is off every time you open the WSPR page.** It used to come back on if that is how you left it last time, which meant a session could start transmitting without you deciding to.
- **The first burst of a session can no longer go out at full power** *(Roy KI0ER)*. With "Protect finals" on, the reduction is confirmed by the radio before anything is armed, rather than the first transmission going out while the request was still in flight.
- **The countdown on the TX button counts down to a real transmission.** It used to count down to the next opportunity and start again whenever the duty cycle decided not to transmit, which made it useless. The decision is now taken in advance, so the time shown is the time until a burst actually happens.
- **The radio gets its power back after an interrupted session** *(Roy KI0ER)*. Leaving the WSPR page has always restored the maximum PA voltage, but a power cut is not leaving the page — so a session interrupted overnight could leave the radio at about a quarter power in every mode, with nothing on screen saying so. It is now restored when the radio reconnects, and only if the radio is still sitting at the reduced voltage, so it can never push one radio's setting onto another *(Michael KZ4LY)*.
- **The spot list no longer says "Listening..." while transmitting** *(Roy KI0ER)*, and the waterfall stands completely still for the transmitting cycle instead of scrolling blank.
- **The transmit controls are in one place.** "Allow transmitting" has gone from the settings drawer and from the web settings window: it set the same thing as the TX button on the WSPR page and did not track it, so the two could disagree.
- **The TX button moved to the bottom of the panel** *(Randy N4OPI)*, clear of the edge used for swiping between screens.

**FT8 and FT4**

- **The band dropdown no longer switches you to FT4 by mistake** *(Steve KX7R)*. In v1.10.6 the browser's FT4 frequency list was accidentally filled with the FT8 frequencies, so whichever preset you picked switched the radio to FT4 — and there was no way back from the browser. The dropdown now has separate FT8 and FT4 groups, and the mode shown comes from the radio rather than being guessed.

**Web page**

- **A large heading says which screen you are on** — MODE: FT8, MODE: FT4 or MODE: WSPR — matching the Tab5.
- **The FT8 page shows the slot occupancy strip and the slot countdown**, the same two the Tab5 has always had: which 50 Hz slots are busy in each transmit window, and how long the current slot has left.
- **The WSPR page has the whole left-hand panel from the Tab5**: band selection, the TX button and its state, the finals-guard voltage, best DX, stations heard per cycle, and the wsprnet publishing status. Previously it showed only the list of decodes.
- **The layout stops moving.** Both panels reserved no space, so everything shifted as decodes arrived; the receive status, decode count and controls now keep one position.

**Smaller things**

- Distances and bearings, the decode count and the receive status are all readable rather than fine print.
- The mid-QSO buttons always take their own line rather than moving depending on window width.

### Shipped in v1.10.8 — 2026-09-03

**A crash introduced and fixed in the same release cycle, plus more groups.io reports.**

**Reliability**

- **A settings-drawer scroll could crash the device.** v1.10.7's "while the drawer is scrolling, nothing else in it acts" fix called into the touch driver to force a scroll gesture to be treated as already released the moment it began, so a slider or the close-swipe could not steal it. That collided with LVGL's own built-in momentum-scroll animation running on the same drawer object: every reproduction crashed the display task at the identical point, inside LVGL's own animation-completion code, with the animation's callback pointer corrupted to a small, obviously-wrong value. A diagnostic build that guarded the crash site and logged instead of jumping into it confirmed this was one specific interaction happening every time, not a rare race, before anything was changed. The offending call is removed; the drawer's scroll-vs-tap protection is unchanged and still works, because sliders and checkboxes were never actually at risk from it — they cannot chain a touch into the drawer's own scroll in the first place.
- **The web spectrum could go on drawing against a stale frequency axis** *(Samuel W7STF)*. Spectrum data and the frequency axis it is drawn against arrive on two different channels — the spectrum over a fast stream, the axis on a once-a-second status poll — so a single missed poll after a band change or mode switch used to leave the picture looking completely normal while every signal sat at the wrong frequency. It now blanks itself and says why after a few missed polls, instead of showing a plausible lie. Not yet confirmed on the air.
- **A WSPR spot heard just before a band hop could be published to wsprnet.org under the wrong band** *(Kevin KQ4DTX)*. The upload read the dial at send time, several minutes after the fact, rather than the dial the spot was actually heard on — so with band hopping and wsprnet publishing both on, a station heard on one band could be reported on another in a public database, with no way for anyone reading it to know. It now uses the frequency recorded at the moment of the decode. Not yet confirmed on the air.

**QSO logging**

- **Restore worked-station history from a downloaded ADIF file** *(Randy N4OPI)*. An erase-and-reinstall used to leave no way back to it — restoring your settings has never touched the QSO log, on purpose, so a config restore could not help either. "ADIF restore" in the web UI's QSO Logs menu merges a previously downloaded (or any other logger's) ADIF file in, skipping any contact already logged, and marks the restored contacts as already uploaded to QRZ/eQSL/LoTW so they are not sent again.

**Web page**

- **The decode list no longer jumps up and down during an exchange** *(Randy N4OPI)*. The status box above the list grew and shrank with the message text, pushing the list below it around while you were trying to click a row. It now holds a fixed height.

**Smaller things**

- The "QMX cannot display this" caption is gone from the hatched dead-band on both the Tab5 and the browser — the hatching alone says what it needs to.

### Shipped in v1.10.9 — 2026-09-05

**The web decode-list jump, root-caused for real this time; WSPR's PA-voltage guard made reliable; a remote QMX power-cycle relay.**

**Web page**

- **The decode list no longer jumps, for real this time** *(Randy N4OPI)*. v1.10.8's fix sized the status box against one "worst case" message and still moved on an armed transmit or a busy exchange — the real cause was the box disappearing from the page entirely while idle and reappearing at full size the moment there was something to show, not its size while visible. Every place that used to hide it now leaves its space reserved and only hides what is drawn inside it, so idle and busy are the same height throughout. The countdown shown while a transmission is armed is now its own small figure that cannot be cut off by a long message, Cancel clears the box immediately with no leftover "Cancelling" text, and the box no longer runs wider than the slot occupancy strip below it.
- **The "Calling you" pileup list now ages out and clears on a band change** *(Randy N4OPI)*. It previously had no expiry at all — a caller from 17 hours earlier, on a different band, was still shown. Entries now drop after an hour, and changing bands clears the list outright, the same moment that already ends an in-progress QSO for the same reason.

**WSPR**

- **The finals-protection PA-voltage guard is now confirmed and retried, not fire-and-forget** *(Dirk DK7CVD)*. Restoring the radio's power when leaving WSPR mode was a single CAT write with nothing checking it actually landed — if that one write was lost, the radio stayed at the reduced WSPR voltage indefinitely, in every mode, with nothing on screen saying so. A background check now confirms the restore took and resends it if not. Verified end to end on real hardware: reduced 11.5 V to 6.0 V on WSPR TX, held there correctly for the whole session, and restored 6.0 V back to 11.5 V on leaving WSPR, each step confirmed against the radio's own CAT read-back.
- **The WSPR countdown no longer appears to hit zero and restart on the first transmit cycle.** The radio's PA-voltage was only asked for once the first cycle began, so a burst due right at the start of a session could be held back a cycle while waiting for that answer — the right call, but the countdown had already been shown counting down to it. The question is now asked the moment WSPR transmit is turned on, giving it the whole two minutes to be answered instead of a few hundred milliseconds.

**New**

- **A remote relay pulse for power-cycling the QMX** *(Randy N4OPI)*. "Power-cycle relay" under the web UI's Miscellaneous menu drives one of two Tab5 GPIO pins for a chosen level and duration — wire a home-automation relay to it and the QMX's PWR_ON/GND **signals**, and a remote firmware upgrade (which always needs the QMX power-cycled afterward) no longer needs someone physically at the radio. (The QMX has no PWR_ON/GND jack - those signals must be brought out to a connector of your own, so this is an experimenter feature; wording corrected in v1.11.1 after Randy N4OPI pointed out someone would hunt for a socket that does not exist.)

### Shipped in v1.11.0 — 2026-09-05

**Your QSO log, recoverable from the card it was already backed up to — and searchable on both screens.**

All of it from one report. Gyula HA3HZ chose the erase option when updating his
firmware, having first saved his log, and then found no way to get 432 contacts
back onto the device. Two of my own explanations for that were wrong and were
tested against his actual files on real hardware before the third was found: the
importer was never at fault, because his log never reached it. The microSD
archive had only ever been able to write *to* the card.

**Restoring the log**

- **Restore straight from the microSD card.** The card has always held a copy of the QSO log, and the Tab5 could only ever write to it — so recovery needed a computer, a browser, and knowing the file was on the card at all. It reads it back now: **Restore from SD** in the Tab5's own log window (no computer, which is the POTA case), or **↳ Restore from SD card** in the browser's QSO Logs menu. Both merge, so contacts already logged are skipped, nothing is duplicated, and pressing it twice does nothing.
- **The previous log is kept as `qso.prev.adi`.** The card mirrors the *present*, so a contact deleted before a restart was gone from the card too at the next start-up — which is exactly what happened on the bench while testing this, and the records only survived because a copy happened to be sitting on the build machine. Whenever the log about to be written is **smaller** than the one on the card, the older copy is kept first. Growth is ordinary logging and never disturbs it, so it holds the last larger version for as long as it takes to notice. Recover it with the file browser and **ADIF restore ↑**.
- **An import now says what actually happened.** Reporting only how many contacts were added cannot distinguish "all of these were already logged" from "not one of them could be read", and the page asserted the first in both cases. It now reports found, added, already present and unreadable. A file over the size limit gives a plain sentence rather than a raw browser error; the limit itself went from 256 KB to 1 MB (about 1200 QSOs to roughly five thousand); and an upload interrupted by a slow link is retried instead of abandoned.

**Searching and exporting**

- **Search the log, on the Tab5 and in the browser.** Any part of a callsign, country, band, mode, date, grid or park reference; several words must all match, so `ha3 20m` narrows to HA3-prefix contacts on 20 m. A search that finds nothing says so in as many words — *"so this one has not been worked"* — because that is the question the box exists to answer. The decode list already greys out a worked station, but only while that station is on the air; this asks the same question whenever you like. Country is searchable although no ADIF field stores it: it comes from the same prefix lookup the Country column shows, so the two cannot disagree.
- **The Tab5's log gains a Ref column** — the park or summit the other station was activating. Added because the search offered to find it while the list never showed it, and a column you can search but cannot see is a promise the screen does not keep. The window is wider to fit it.
- **Export just the contacts you pick** (browser). Tick rows and press **Export selected** to save them as their own ADIF file. The tick box in the header takes everything **currently shown**, so a search plus one tick gives a single day, band or park as a file. Each record is exported exactly as the Tab5 wrote it, rather than rebuilt from the columns on screen, so fields the table does not display survive the trip.

**Messages that wait to be read**

- **A finished QSO stays on screen in the browser** *(Randy N4OPI)*. `<callsign> QSO complete` in green, held until you do something else, where every other message in that line clears itself after twenty seconds. Stepping away and coming back now tells you the contact finished. Two other designs were rejected first: holding the radio in the completed state blocks the next QSO, and freezing the display misreports what the radio is doing. This is neither — it is a record that the contact happened, and the radio carries on regardless. The Tab5's own label is unchanged; it is the browser that has to cope with nobody watching.
- **Results on the Tab5 are a window with an OK button**, not a message that fades. A restore, a delete of test contacts, or clearing the whole log all report in a panel you dismiss — the outcome of something you asked for is worth reading, and dismissing it is how you say you read it. Clearing the log spells out "This cannot be undone".

