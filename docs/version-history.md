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

---

*This is the archived "Shipped in" history. The live roadmap (Next up / Longer term) is in [`README.md`](../README.md).*

