# QMX Panadapter — Master Todo List + Status Assessment

**Last updated:** 2026-06-29
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
| **v1.0 Release Gates (2 blockers for stable release)** | | | | |
| 1 | LoTW upload (TQSL) | ❌ Not started | Design→Code | Certificate-based via TQSL, not a simple HTTP API like QRZ/eQSL. QRZ+eQSL already shipped (v0.16.2); LoTW is the last logging target. mbedtls→PSRAM fix already unblocked outbound HTTPS, so the TLS issue QRZ hit won't recur — look elsewhere first if LoTW fails to connect. Design architecture first |
| 2 | FT8 TX multi-day soak | ⚠️ Code ready, testing only | Testing only | No duty-cycle protection, no audio loopback verification, no over-temperature monitoring yet. All RX/panadapter/web/logging features already stable. Blocks dropping the "beta" label |
| **Groups.io Feature Requests** | | | | |
| 3 | One-button TUNE (WS1M/bammi, Jun 25) | ⚠️ Verify needed | ? | Single button to trigger external relay/tuner function. Possibly already discussed/handled in groups.io message #172521 — needs verification |
| 4 | CW page — Phase 1: TX/memory (thread, Jun 24-25) | ❌ Not started | Medium | Canned-message buttons triggering CW TX, reusing existing `mem_channels.c` + CAT key-down plumbing (same pattern as FT8 CQ presets). No new DSP, no audio dependency — ship standalone |
| 4b | CW page — Phase 2: RX decode | ❌ Not started | Large, or cheap if CAT-mirror works | Scrolling decoded CW text under the spectrum. Option (a) Goertzel tone tracker (new DSP, Large); option (b) mirror QMX's own internal decode via CAT (cheap — check CAT manual for a CW-decode query before committing to (a)). Does NOT need shelved CW Audio (#6) — only needs the RX ring buffer, already flowing for FT8 |
| 5 | FT4 mode (Roy, Jun 26) | ✅ Shipped (v0.19.0) | — | RX engine + TX cadence verified on hardware; both modes live. Was the cheap trial run for generalizing the hardcoded-15s slot machinery — JS8 (#10) benefits from that same generalization |
| 5.5 | FT4 time sync | ❌ Not started | Medium | 7.5s slot timing estimation in `ft8_time_modal.c` is currently FT8-only (15s). Hide "Sync Time" button in FT4 mode until implemented |
| **Shelved Work** | | | | |
| 6 | CW Audio (speaker/headphone output only) | ✋ Shelved (v0.18.5/.6) | Unblock needed | Disabled to restore FT8 decode performance. Root cause: `cw_audio_preopen()`+`dsp_cw_forward()` degraded yield 2–3× even with audio off (v0.18.5); `cw_audio_init()` also spawned a priority-6 ghost task on core 1 preempting `fft_task` ~125×/slot, found+disabled in v0.18.6, confirmed via controlled A/B to fully restore v0.18.0-level yield. Fix needed before re-enabling: root-cause the original I2S/DMA/UAC contention AND fix task priority/cadence, then soak-test a full session. Blocks #7 only — does NOT block CW Phase 2 decode (#4b) |
| **Longer-Term Roadmap (Post v1.0)** | | | | |
| 7 | Speaker/headphone audio (Tab5 jack) | ❌ Not started, blocked | Large | Demodulated CW/SSB passband audio from Tab5's own jack. Blocked on unshelving CW Audio (#6) |
| 8 | Extended waterfall history | ❌ Not started | Medium | PSRAM has room for several minutes of scrollback; two-finger drag to scrub through |
| 9 | QMX (small) support | ❌ Not started | Medium | Same UI, different USB endpoint config and band table |
| 10 | JS8 mode | ❌ Not started | Large | Heavily reuses `ft8_lib` per feasibility doc; has 10/15/30/60s slot variants. Do after FT4 (#5) to reuse its slot-abstraction work. See `docs/js8-feasibility.md` |
| 10b | RTTY mode | ❌ Not started | Large | Fully separate pipeline — no LDPC, no block structure. Unrelated to FT4/JS8 work. See `docs/rtty-feasibility.md` |
| 11 | DSP polish (noise reduction, auto-notch) | ❌ Not started | Medium | New algorithms, no design started |
| **Groups.io Feedback — Jun 29 (Ken KF0AYY, Dirk)** | | | | |
| 12 | FT8 sim phantom sent grid instead of report at WAIT_RPT | ✅ Fixed (unreleased) | — | Fixed in `ft8_sim.c` — was sending `ph->grid` again instead of a signal report, contradicting `ft8_qso.c`'s own WAIT_RPT expectation. Caused Ken's "infinite grid loop" in Practice Mode. Built + verified, not yet flashed/released |
| 13 | Persistent multi-target CQ pile-up list (Ken) | ❌ Not started | Large | Real architecture change — `ft8_qso.c` needs N-way exchange tracking (not just single `s_target`), plus a pile-up panel UI in `ft8_screen_view.c` (tap-to-focus, swipe-to-remove, auto-remove on RR73 sent). Scope properly before starting |
| 14 | FT8 row touch-and-hold selection unreliable (Ken + Dirk, two reports) | 🔴 Identified bug | Medium | `ft8_screen_view.c`: list stays `LV_OBJ_FLAG_SCROLLABLE` for the first `ROW_HOLD_SELECT_MS` (250ms), so LVGL's native scroll/kinetic logic races our own gesture gate on the same touch. Consider Dirk's tap-then-confirm alternative instead of hold-to-select |
| 15 | FT8 list scrolling "fights itself" (Dirk) | 🔴 Identified bug | Small–Medium | Same root cause as #14 — fix together |
| 16 | FT8 reply latency — decoded reply lands a cycle late, stale repeat sent meanwhile (Dirk, clarified) | 🟠 Open investigation | Medium | Confirmed architectural: `ft8_qso_advance()` runs ~4s into the *next* slot (per its own design comment in `ft8_qso.c`), often after that slot's TX decision is already made. The `FT8_REPLY_TX_WINDOW_MS` (2500ms) optimization exists for exactly this but only catches it if decode finishes within 2.5s of slot start. Need to measure actual decode latency against that window (Dirk is sending a diag log) before deciding to widen the window or speed up decode further |
| 17 | FT8 confirm dialog: add up/down nudge buttons (Dirk) | ❌ Not started | Small | If the wrong row gets selected, let the operator nudge ±1 row from the confirm modal instead of re-doing the hold gesture. `row_activate(idx)` in `ft8_screen_view.c:389` is already keyed purely off row index — two buttons on the modal calling `row_activate(idx±1)` is a small, self-contained addition. Natural pairing with #14/#15 |
| 18 | I/Q image artifact after QMX+ menu visit, only clears on QMX power-cycle (Dirk, w/ screenshot) | 🔴 Identified likely cause | Medium | Screenshot shows a classic twin-mirrored-band I/Q image, not a QMX test tone. Likely cause: `iq_balance.c`'s fast-reconverge window only triggers on a zero-byte/silence reset detection (shared with the flat-spectrum-floor reset), which a menu visit may not produce if the audio stream doesn't actually go silent. Needs a broader "reconverge" trigger, not just the power-cycle workaround |
| 21 | FT4 QSOs logged to ADIF as MODE=FT8 | ✅ Fixed (unreleased) | — | `ft8_qso.c:747` hardcoded `.mode = "FT8"` on every completed QSO regardless of actual sub-mode. Now reads `ft8_op_mode_get()` and logs "FT4" correctly. Built+verified, not yet flashed/released. Note found while tracing this: FT4 currently has no auto-reply/exchange logic at all (`ft8_test.c` — "FT4 currently only supports CQ, no auto-reply"), so real FT4 QSOs via the auto QSO machine aren't actually happening yet — worth flagging to whoever reported this |
| **Closed Investigations** | | | | |
| 19 | FT8 decode-yield gap to v0.18.0 | ✅ Closed (2026-06-26) | — | Three real regressions found+fixed since v0.18.0 (cw_audio ghost task, d140485 partial-revert restoration, unconditional opacity-set in `ui_push_spectrum()`). Controlled same-time-of-day A/B (chip-erased, 60-slot captures): v0.18.0 vs HEAD both 15.38 decodes/slot mean, HEAD tighter stddev (5.70 vs 6.65). Earlier "gap" was a band-fading confound, not a code regression. Full methodology in memory `project_ft8_sparse_decode_investigation` |
| **Closed/Shipped — Full Version History** | | | | |
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
