# QMX Panadapter — Master Todo List + Status Assessment

**Last updated:** 2026-06-30
**Scope:** v1.0 release gates → open investigations → feature requests → roadmap → full shipped history
**Source:** CLAUDE.md + README.md + groups.io feature requests + session work
**Assessment:** Code grep + git log + memory system

---

## ⚡ Status Legend

- 🔴 **Identified bug** — Code defect confirmed, ready to fix
- 🟠 **Open investigation** — Partially fixed, real gap remains, root cause not fully closed
- ❌ **Not started** — Genuinely new, no code/design
- ⚠️ **Needs verification** — Partial info or prior work unclear
- ⏸️ **Partial/Designed** — Infrastructure exists or design done, coding pending
- ✋ **Shelved** — Intentionally disabled, unblocking needed
- ✅ **Shipped** — Already completed and released

---

## 📋 Master Table

| # | Item | Status | Effort | Next Step / Notes |
|----|------|--------|--------|-----------|
| **👤 User / Field Requests (all groups.io + named-reporter items, one group)** | | | | |
| 3 | One-button TUNE (WS1M/bammi, Jun 25) | ⏸️ Blocked on QMX `1_04` (see #22) | Small once unblocked | A single on-screen button to trigger the QMX's SWR Tune mode — currently that's buried deep in the QMX's own menu structure, this would surface it directly from the panadapter UI. Real implementation path: `1_04`'s new CAT command `MD8;` enters SWR Tune mode (`MD0;` exits) — see the `1_04` changelog notes under #22. Welcome, easy-to-justify feature once `1_04` is verified on real hardware; blocked on the same hardware-access gap as #22, not on design uncertainty |
| 4 | CW page — Phase 1: TX/memory (thread, Jun 24-25) | ❌ Not started | Medium | Canned-message buttons triggering CW TX, reusing existing `mem_channels.c` + CAT key-down plumbing (same pattern as FT8 CQ presets). No new DSP, no audio dependency — ship standalone |
| 4b | CW page — Phase 2: RX decode | ❌ Not started | Large, or cheap if CAT-mirror works | Scrolling decoded CW text under the spectrum. Option (a) Goertzel tone tracker (new DSP, Large); option (b) mirror QMX's own internal decode via CAT (cheap — check CAT manual for a CW-decode query before committing to (a)). Does NOT need shelved CW Audio (#6) — only needs the RX ring buffer, already flowing for FT8 |
| 5 | FT4 mode (Roy, Jun 26) | ✅ Shipped (v0.19.0) | — | RX engine + TX cadence verified on hardware; both modes live. Was the cheap trial run for generalizing the hardcoded-15s slot machinery — JS8 (#10) benefits from that same generalization |
| 5.5 | FT4 time sync | ✅ Shipped (2026-06-30) | — | Turned out to be a real bug, not a missing feature: timing-offset calc hardcoded FT8's block geometry (1920/960 samples) regardless of protocol, silently wrong (~3.3x) for FT4 (576/288). Fixed by reading `mon->block_size`/`mon->subblock_size`; FT8-only auto-sync gate removed now that the math is correct for both. Follow-up UI fixes same day: bottom-bar clock now shows `UTC(FT4)` not `UTC(FT8)` while in FT4; Filter modal's "Sync Time" button (previously hidden in FT4) restored; "Set and Sync the Clock" modal hint/flash/log text now says FT4 when appropriate. Verified on hardware. See CLAUDE.md "FT4 clock-sync timing" |
| 12 | FT8 sim phantom sent grid instead of report at WAIT_RPT | ✅ Shipped (v0.19.2) | — | Fixed in `ft8_sim.c` — was sending `ph->grid` again instead of a signal report, contradicting `ft8_qso.c`'s own WAIT_RPT expectation. Caused Ken's "infinite grid loop" in Practice Mode |
| 13 | Persistent multi-target CQ pile-up list (Ken) | ❌ Not started | Large | Real architecture change — `ft8_qso.c` needs N-way exchange tracking (not just single `s_target`), plus a pile-up panel UI in `ft8_screen_view.c` (tap-to-focus, swipe-to-remove, auto-remove on RR73 sent). Scope properly before starting |
| 14 | FT8 row touch-and-hold selection unreliable (Ken + Dirk, two reports) | ✅ Shipped (2026-06-30) | — | `8317e0e` — replaced hold-to-select with tap-to-select in `ft8_screen_view.c`, eliminating the race between LVGL's native scroll and the custom gesture gate |
| 15 | FT8 list scrolling "fights itself" (Dirk) | ✅ Shipped (2026-06-30) | — | Same fix as #14 (`8317e0e`) — root cause was shared |
| 16 | FT8 reply latency — decoded reply lands a cycle late, stale repeat sent meanwhile (Dirk, clarified) | 🟠 Open investigation, partial fix shipped (v0.19.2) | Medium | Confirmed and measured from a real Dirk QSO log: `cap≈15.1s` + `dec≈1.5s` exceeds the 15s slot, so a continuation reply routinely finishes just after the previous message has already been re-fired — guaranteed on a busy band, not occasional, since most of ~140 sync candidates are false positives that always burn the full LDPC iteration budget before failing. `FT8_LDPC_MAX_ITERS` cut 30→15 in v0.19.2, ~25-30% dec_ms reduction on hardware. Does not fully close the gap — capture alone already overshoots the slot by ~100ms before decode starts; fully closing it means trimming the capture margin itself (risks reopening the v0.15.1 clock-drift bug). Next step if pursued further: trim capture margin + re-verify against both weak-signal decode rate and resend frequency |
| 17 | FT8 confirm dialog: add up/down nudge buttons (Dirk) | ✅ Shipped (2026-06-30) | — | `0b6fb84` — `ft8_screen_view_nudge_confirm(delta)` lets the TX confirm modal re-target ±1 row without redoing the hold/tap gesture; also added a Transmit(green)/Auto Pounce(blue) color-swatch legend |
| 18 | I/Q image artifact after QMX+ menu visit, only clears on QMX power-cycle (Dirk, w/ screenshot) | ✅ Closed — root cause was QMX `1_04` beta, not `iq_balance.c` | — | Not an `iq_balance.c` reconverge bug as originally suspected. Real cause: Dirk was on QMX `1_04` beta firmware, which silently failed to actually enable IQ mode despite our `Q9 1;` write reporting success at the CDC layer — without real IQ mode the QMX outputs a mirrored-image signal, which is exactly the twin-mirrored artifact in his screenshot. Addressed by the `Q9;` readback fix (v0.19.2, see CLAUDE.md) that now detects and logs this instead of silently trusting the write, plus the existing guidance to use `1_03_002` until `1_04` is verified (see #22) |
| 21 | FT4 QSOs logged to ADIF as MODE=FT8 | ✅ Shipped (v0.19.2) | — | `ft8_qso.c:747` hardcoded `.mode = "FT8"` on every completed QSO regardless of actual sub-mode. Now reads `ft8_op_mode_get()` and logs "FT4" correctly. Note: FT4 currently has no auto-reply/exchange logic at all (`ft8_test.c` — "FT4 currently only supports CQ, no auto-reply"), so real FT4 QSOs via the auto QSO machine aren't actually happening yet — worth flagging to whoever reported this |
| 23 | Display warm above USB-A port during operation (Samuel W7STF, groups.io #172940, Jun 30) | ℹ️ Status quo, no action | — | Expected — that's roughly where the Tab5's own USB host silicon sits, and we run USB host (UAC+CDC-ACM) plus WiFi continuously. Not something we control or that indicates a problem |
| 24 | Configurable FFT size / PSD refresh rate / waterfall speed / draggable spectrum-waterfall split (Samuel W7STF, groups.io #172940, Jun 30) | ❌ Not started | Large (multiple distinct asks bundled) | Three separate enhancement requests in one post, each independent: (a) selectable FFT size — affects `dsp.c`'s `DSP_FFT_SIZE` and downstream bin math (IF-shift, peak search, waterfall column width) used throughout `render.c`/`render_waterfall.c`/`ui.c` — touches a lot of bin-index assumptions, needs careful audit, not just a config knob; (b) adjustable PSD/waterfall refresh rate — `render_task` is currently a fixed 30 Hz tick, probably the easiest of the three to make configurable; (c) draggable spectrum/waterfall split with point-and-drag gesture — new touch-drag UI affecting the fixed 200px/412px panel heights in the landscape layout (`CLAUDE.md` "Display layout"), would need layout to become dynamic instead of compile-time fixed regions. Scope each sub-item separately before starting; (b) is the cheap one if only one ships first |
| 25 | Freq-entry popup: persistent position ✅ shipped; selectable size still open (Samuel W7STF, groups.io #172940, Jun 30) | 🟡 Half shipped (2026-06-30) | Medium remaining | Part (b) done: drag the popup by its "Enter freq" title label (the only non-button area) to reposition anywhere on screen, clamped to stay fully visible; position persists via `settings_set_freq_kp_pos()` (debounced flush, writes once on release) and is restored on next open instead of re-centering. Deliberately kept out of `DIRTY_CONFIG_EXPORT_MASK` — cosmetic placement, not worth an SD-mirror write. Shared by both the top-bar keypad and the Memory-channel picker (`ui_freq_picker_open()`), since both use the same popup. Verified on hardware. Part (a) — selectable/smaller size — still not started; needs the keypad layout to support more than one fixed size, larger scope than the position fix |
| 27 | Waterfall display shifted right of the spectrum trace, intermittent (Dirk DK7CVD, groups.io #172933, Jun 30 12:05) | 🔴 New bug report, not yet investigated | Medium, needs repro | Dirk: "I had another weird behaviour and it just happened again, but I can't fully reproduce it... The waterfall display is shifted to the right. I can hear the FT8, it's showing the correct frequency, but the signal in the waterfall is at the wrong location." Screenshot shows the spectrum trace (green curve + amber VFO cursor) correctly centered at 14.074.000 MHz, but the waterfall paint below it is offset horizontally from the spectrum/freq-axis alignment — the two panes should share the same X-axis bin mapping (`render.c` writes both from the same FFT output) but appear desynced. Intermittent + not reproducible on demand makes this hard to chase blind; likely candidates worth checking first: zoom-FFT pan/recenter state (`recompute_zoom_pan()`) going stale relative to the waterfall's own column-write offset, or a stale `ui_get_if_bin_shift()` value if mode/bandwidth changed without both panes' offset state being recomputed together. Ask Dirk for: was zoom >1x active, had mode/bandwidth just changed, does re-entering zoom or toggling Panadapter↔FT8 clear it (consistent with a stale-recompute theory) |
| **v1.0 Release Gates (2 blockers for stable release)** | | | | |
| 1 | LoTW upload (TQSL) | ❌ Not started | Design→Code | Certificate-based via TQSL, not a simple HTTP API like QRZ/eQSL. QRZ+eQSL already shipped (v0.16.2); LoTW is the last logging target. mbedtls→PSRAM fix already unblocked outbound HTTPS, so the TLS issue QRZ hit won't recur — look elsewhere first if LoTW fails to connect. Design architecture first |
| 2 | FT8 TX multi-day soak | ⚠️ Code ready, testing only | Testing only | No duty-cycle protection, no audio loopback verification, no over-temperature monitoring yet. All RX/panadapter/web/logging features already stable. Bench session 2026-06-29 verified multiple full auto-pounce QSOs completing correctly end-to-end (CQ→TX1→RPT→RR73→73→ADIF log) post-v0.19.2, but this is still single-session evidence, not the multi-day soak this gate requires. Blocks dropping the "beta" label |
| 22 | QMX `1_04` firmware compatibility + new features it enables | ⚠️ Changelog pulled (2026-06-30), hardware testing still needed | Medium–Large, needs scoping | Still 3 betas, no GA: `1_04_000` (08-May, QMX+ only), `1_04_001` (12-Jun), `1_04_002` (18-Jun) — fetched full changelog from qrp-labs.com/qmx, see `CLAUDE.md`. Real CAT additions not yet handled: `MD8;` SWR Tune mode + `MD` mode-5=AM (our mode table maps both to unrecognized); `MU;`/`PS`/`KD`/`TR`/`RR` unimplemented (no current use case). `AI` (Auto Info) was reworked with proper modes in `1_04_000` — possibly relevant to the `Do not call AI1;` landmine in this doc, but **must be verified on real `1_04` hardware, not assumed fixed from the changelog**. Also: `1_04_001` claims a fix to "CAT MU command and MM loaded previously saved state" — might interact with our SSB-filter-bandwidth `FW;`-suppression workaround; flag for re-check once hardware is available, don't simplify blind. Beyond CAT, `1_04` adds AM RX mode, "Virtual U3S" beacon mode, SSB "Symmetric phase" quality fix, fullscreen CW practice decode — all QMX-native, not currently surfaced by us. Next step: get a real `1_04` unit for testing — changelog research alone can't verify behavior. Blocks #3 (one-button TUNE, wants `MD8;`) |
| **Shelved Work** | | | | |
| 6 | CW Audio (speaker/headphone output only) | ✋ Shelved (v0.18.5/.6) | Unblock needed | Disabled to restore FT8 decode performance. Root cause: `cw_audio_preopen()`+`dsp_cw_forward()` degraded yield 2–3× even with audio off (v0.18.5); `cw_audio_init()` also spawned a priority-6 ghost task on core 1 preempting `fft_task` ~125×/slot, found+disabled in v0.18.6, confirmed via controlled A/B to fully restore v0.18.0-level yield. Fix needed before re-enabling: root-cause the original I2S/DMA/UAC contention AND fix task priority/cadence, then soak-test a full session. Blocks #7 only — does NOT block CW Phase 2 decode (#4b) |
| **Longer-Term Roadmap (Post v1.0)** | | | | |
| 7 | Speaker/headphone audio (Tab5 jack) | ❌ Not started, blocked | Large | Demodulated CW/SSB passband audio from Tab5's own jack. Blocked on unshelving CW Audio (#6) |
| 8 | Extended waterfall history | ❌ Not started | Medium | PSRAM has room for several minutes of scrollback; two-finger drag to scrub through |
| 9 | QMX (small) support | ❌ Not started | Medium | Same UI, different USB endpoint config and band table |
| 10 | JS8 mode | ❌ Not started | Large | Heavily reuses `ft8_lib` per feasibility doc; has 10/15/30/60s slot variants. Do after FT4 (#5) to reuse its slot-abstraction work. See `docs/js8-feasibility.md` |
| 10b | RTTY mode | ❌ Not started | Large | Fully separate pipeline — no LDPC, no block structure. Unrelated to FT4/JS8 work. See `docs/rtty-feasibility.md` |
| 11 | DSP polish (noise reduction, auto-notch) | ❌ Not started | Medium | New algorithms, no design started |
| **Closed Investigations** | | | | |
| 19 | FT8 decode-yield gap to v0.18.0 | ✅ Closed (2026-06-26) | — | Three real regressions found+fixed since v0.18.0 (cw_audio ghost task, d140485 partial-revert restoration, unconditional opacity-set in `ui_push_spectrum()`). Controlled same-time-of-day A/B (chip-erased, 60-slot captures): v0.18.0 vs HEAD both 15.38 decodes/slot mean, HEAD tighter stddev (5.70 vs 6.65). Earlier "gap" was a band-fading confound, not a code regression. Full methodology in memory `project_ft8_sparse_decode_investigation` |
| **Closed/Shipped — Full Version History** | | | | |
| 28 | Memory-channel grid: drag-to-move, mode colours, validation, UX polish | ✅ Shipped (2026-06-30) | — | `066096d`. Long-press + lift = edit (was immediate on press-down); long-press + drag = move a channel to an empty slot, snaps on release; empty-slot tap goes straight to the editor. Out-of-band frequencies refused via new `ui_validate_band_freq_hz()`, checked the instant the freq pad is confirmed — required changing `ui_freq_picker_cb_t` to return bool so a reject can leave the pad open instead of closing out from under the user. Label keyboard starts in "Abc" (capitalize-first) state. Empty slot + no QMX shows "Enter freq" not "0.000.000 Hz". Button label now shows freq before mode. New shared `ui_theme_mode_color()` colours the grid + the freq pad's own DiGi/USB/LSB/CW row identically; CW/DiGi aligned to the band-plan strip's existing colours, USB changed from steel-blue (too close to CW) to brick red/brown, all four (+ band-plan Phone) dimmed ~30%, band-plan label text switched to white to stay legible against the dimmer fills. A top-bar Mode-text colour change was tried and explicitly reverted same session — left at its original static green |
| — | v0.19.2 — Crash fix: USB CDC disconnect race | ✅ Shipped | — | `link_task`'s disconnect cleanup closed the CDC handle while `poll_task` could still be mid-retry on it (v0.18.6's 20-strikes tolerance widened the window). Root-caused from a Dirk DK7CVD serial capture reproducing the crash after the QMX's SWR Tune submenu + power-cycle. Fixed: `link_task` now waits (bounded, 4s) for `poll_task` to exit before closing |
| — | v0.19.2 — QMX IQ mode now verified via `Q9;` readback | ✅ Shipped | — | Previously only checked that the `Q9 1;` USB write succeeded, never that the QMX actually accepted it. Surfaced a real case on `1_04` beta firmware where IQ mode read as still-disabled in the QMX's own menu |
| — | v0.19.2 — QMX `1_04` beta firmware flagged as unverified | ✅ Shipped (docs) | — | quick-start/README/troubleshooting now pin guidance to `1_03_002` and call out `1_04` as untested, instead of implying "newer is better" |
| — | v0.19.2 — FT8 continuation-message resend reduced | ✅ Shipped | — | `FT8_LDPC_MAX_ITERS` 30→15; see item #16 above for full root cause |
| — | v0.19.2 — microSD auto-archive | ✅ Shipped | — | Diag log/ADIF/config export mirrored automatically to `/sdcard/qmx-panadapter/` when a card is present; breathing SD dot in bottom bar |
| — | v0.19.2 — Diagnostic log always-on + persists across power loss | ✅ Shipped | — | Was opt-in + RAM-only; now captured from boot and rolled to internal flash, downloadable even with no SD card ("Diag(saved) ↓") |
| — | v0.19.2 — SD-card screenshot save removed | ✅ Shipped | — | Prototyped alongside the SD archive, removed before release — collided with the WiFi co-processor over a shared SDMMC peripheral and reliably killed WiFi. Web-based screenshot unaffected |
| — | v0.19.2 — USB host DWC DMA buffers → PSRAM | ✅ Shipped | — | Fixed USB endpoint allocation failure on QMX reconnect-after-WiFi (internal SRAM fragmentation) |
| — | v0.19.2 — Internal-RAM heap fixes (task stacks, FT8/config buffers) | ✅ Shipped | — | 7 background task stacks + FT8 clear-tone-scan buffer + config import/export buffers moved off scarce internal RAM onto PSRAM |
| — | v0.19.2 — LVGL memory pool → PSRAM | ✅ Shipped | — | Freed a fixed 256 KB block of internal RAM previously reserved for LVGL's allocator |
| — | v0.19.1 — tab5.lav.dk homepage launch | ✅ Shipped | — | New project homepage as plain web pages (user guide + reference) |
| — | v0.19.1 — Logbook upload/download reliability fix | ✅ Shipped | — | QRZ/eQSL uploads + log downloads now work reliably while FT8 is running — no more reboots or dropped WiFi during a transfer |
| — | v0.19.0 — FT4 transmit + receive | ✅ Shipped | — | 7.5s slots, 105 symbols, 48ms cadence; CAT cadence verified on real hardware; see #5 above |
| — | v0.18.8 — ARRL Field Day FT8 exchange mode | ✅ Shipped | — | `ftx_message_encode_arrl_fd`/`decode_arrl_fd` in `ft8_lib`; bit layout + 86-entry section table verified byte-for-byte vs WSJT-X `packjt77.f90`; `ft8_qso.c` integration; Filter-modal class/section UI with live TX preview; `CQ FD` auto-tag; ADIF fields |
| — | v0.18.8 — FT8 simulation mode | ✅ Shipped | — | `ft8_sim.c`: two phantom stations (W1AW, K9ZZ) call CQ and reply via real encode→GFSK-synth→decode pipeline; hard TX interlock in `ft8_tx.c`; breathing red border while active |
| — | v0.18.7 — FT8 decode-yield gap CLOSED | ✅ Shipped | — | See Closed Investigations #19 above |
| — | v0.18.7 — FT8 auto-answer robot un-shelved | ✅ Shipped | — | Live TX, permanent on-screen "unattended" disclaimer |
| — | v0.18.7 — CQ tone auto-relocation on clash | ✅ Shipped | — | Was just a warning before; `relocate_cq_tone_if_clashing()` now self-heals by hopping to the nearest clear slot |
| — | v0.18.7 — SNTP/QMX time-priority bug fix + FT8 auto-sync | ✅ Shipped | — | 10-min SNTP-freshness check now gated on actual WiFi/SNTP validity; per-slot FT8 timing average auto-applies to system clock every slot |
| — | v0.18.7 — FT8 own-call highlight cache fix | ✅ Shipped | — | Was stale until a mode bounce; now refreshed every cycle in `rebuild_list()` |
| — | v0.18.7 — FT8 Filter modal checkbox sizing + Priority dropdown restyle | ✅ Shipped | — | All 8 checkboxes made textless+separate-label for uniform sizing |
| — | v0.18.7 — Recovery flasher port auto-detection | ✅ Shipped | — | Was hardcoded COM3 / wrong macOS device glob (W7STF field report) |
| — | v0.18.6 — FT8 decode-yield investigation | ✅ Shipped | — | `cw_audio_init()` ghost task found+disabled, `d140485` fix restoration, opacity-set skip — see #19 |
| — | v0.18.6 — RST_SENT fix in CQ-run | ✅ Shipped | — | Responder sending RR73/73 immediately never set `s_rst_sent`; ADIF logged "599" instead of actual SNR |
| — | v0.18.6 — Distance-in-miles toggle | ✅ Shipped (bugs fixed v0.18.7) | — | `distance_in_miles` setting + drawer checkbox; v0.18.7 fixed drawer-section placement + "KM"/"MI" header not flipping live |
| — | v0.18.6 — Diag-log bottom-bar dot | ✅ Shipped | — | Red breathing dot + "Diag" label in bottom bar |
| — | v0.18.5 — CW audio band-aid | ✅ Shipped | — | `cw_audio_preopen()`/`dsp_cw_forward()` disabled — see CW Audio shelved item #6 |
| — | v0.18.5 — FT8 double-spawn crash guard restored | ✅ Shipped | — | Fast Panadapter↔FT8 toggle could spawn a second `ft8_task`, crashing on shared queue |
| — | v0.18.5 — Bootloader-corruption hotfix + recovery tooling | ✅ Shipped | — | Hotfix flasher build wrote merged firmware to flash address 0x0 instead of 0x10000; recovery release + scripts shipped |
| — | v0.18.4 — Band-plan strip + region selector | ✅ Shipped | — | CW/Digi/Phone colour strip tracking VFO; Auto/R1/R2/R3 region drawer selector |
| — | v0.18.4 — One-finger pan/stroll | ✅ Shipped | — | Replaced two-finger stroll; one-finger horizontal drag slides scope+waterfall, retunes on release |
| — | v0.18.4 — Snap-to-signal toggle + CW snap fix | ✅ Shipped | — | Drawer toggle for `dsp_find_peak_hz_around`; CW search now centres on IF offset, not bare 12kHz |
| — | v0.18.4 — Band-aware worked-before (ADIF) | ✅ Shipped | — | Callsign+band worked-before tracking |
| — | v0.18.4 — FT8 robot shelved (built, not soaked) | ✅ Shipped (shelved), later un-shelved v0.18.7 | — | See robot item above |
| — | v0.18.3 — Waterfall drawer controls | ✅ Shipped | — | Live NVS-persisted black level, contrast, adaptive floor blend, FFT window selector |
| — | v0.18.3 — Display 180° flip | ✅ Shipped | — | For upside-down mounting/cable routing; touch follows automatically |
| — | v0.18.2 — WiFi idle reboot fix | ✅ Shipped | — | SDIO RX switched to RX_NONE recycled mempool; was exhausting internal DMA heap under sustained WiFi RX |
| — | v0.18.2 — Web UI reconnect fixes | ✅ Shipped | — | Stale WS sockets now closed on takeover; tolerates transient send failures instead of tearing down the session |
| — | v0.18.1 — Config backup/restore | ✅ Shipped | — | Web `/api/config` export/import of all settings + memory channels as editable INI text |
| — | v0.18.1 — Clean-flash option | ✅ Shipped | — | Flasher prompts normal vs clean (full chip erase) flash |
| — | v0.18.1 — Memory-recall crash fix | ✅ Shipped | — | Direct LVGL-thread CAT write raced the poll task; now optimistic display + deferred poll-task write |
| — | v0.18.1 — Fast Panadapter↔FT8 toggle crash fix | ✅ Shipped | — | Single-instance guard for `ft8_task` spawn |
| — | v0.18.0 — Streaming STFT decode + dual-core FT8 | ✅ Shipped | — | Waterfall built block-by-block during capture; core-0 helper decodes odd candidates in parallel |
| — | v0.18.0 — Reply-on-immediate-slot | ✅ Shipped | — | Replies fire on the same slot instead of a cycle later, validated on-air |
| — | v0.18.0 — dBm scale restored | ✅ Shipped | — | Right-edge centered labels, normal=dBm / flat=dB-above-floor |
| — | v0.18.0 — Tap-to-tune + memory-channel freq fixes | ✅ Shipped | — | Fixed reversal aliasing past Nyquist; fixed dotless-string truncation bug |

---

## 🎓 Reference Links (not tasks — supporting material)

**Documentation:**
- `README.md` Roadmap section
- `docs/version-history.md` — full per-version changelog (v0.1.0 onward)
- `docs/js8-feasibility.md` / `docs/rtty-feasibility.md` — mode feasibility studies
- `CLAUDE.md` "Branch state" + "Next up" — v1.0 gates, QRZ/eQSL details, CW Audio shelved analysis, FT8 decode-yield investigation

**Memory System:**
- `feedback_groups_io_proper_method.md` — how to extract text from groups.io threads
- `project_cw_audio_blocked.md` — CW Audio shelved (I2S/DMA contention)
- `project_ft8_sparse_decode_investigation.md` — FT8 decode-yield investigation (closed 2026-06-26)
- `project_ft4_mode_request.md` — Roy's FT4 request + effort assessment

**Issue Tracking:**
- GitHub Issues / Discussions
- groups.io QRPLabs thread #119565643
