# QMX Panadapter — Master Todo List + Status Assessment

**Last updated:** 2026-06-26
**Scope:** v1.0 release gates → open investigations → next-up → longer-term roadmap
**Source:** CLAUDE.md + README.md + groups.io feature requests + this session
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

## 📋 Quick Verdict Table

| # | Item | Status | Effort | Next Step |
|----|------|--------|--------|-----------|
| **v1.0 Gates** | | | | |
| 1 | LoTW upload (TQSL) | ❌ Not started | Design→Code | Design architecture first |
| 2 | FT8 TX multi-day soak | ⚠️ Code ready | Testing only | Run validation soak |
| **Groups.io Features** | | | | |
| 3 | One-button TUNE | ⚠️ Verify needed | ? | Check message #172521 |
| 4 | CW page w/ memory | ❌ Not started | Medium–Large | Clarify scope |
| 5 | FT4 mode (Roy) | ❌ Not started | Low–Medium | ft8_lib already has FT4; needs slot-timing plumbing |
| **Shelved** | | | | |
| 6 | CW Audio | ✋ Shelved (v0.18.5/.6) | Unblock needed | Fix priority/cadence of cw_audio_task, then I2S/DMA contention |
| **Longer-Term Roadmap** | | | | |
| 7 | CW decoder (Goertzel) | ❌ Not started | Large | New implementation |
| 8 | Speaker/headphone audio | ❌ Not started (blocked) | Large | Unblock CW Audio first |
| 9 | Extended waterfall history | ❌ Not started | Medium | New feature |
| 10 | Phase 6.3 native-portrait | ⏸️ Designed | **Massive** | Sprint-plan rewrite |
| 11 | QMX (small) support | ❌ Not started | Medium | New USB config |
| 12 | JS8/RTTY modes | ❌ Not started | Large | Feasibility docs first |
| 13 | DSP polish (NR, notch) | ❌ Not started | Medium | New algorithms |
| **Closed/Shipped (since last update)** | | | | |
| — | FT8 decode-yield gap to v0.18.0 | ✅ Closed 2026-06-26 | — | Controlled A/B: HEAD == v0.18.0, 15.38 dec/slot each |
| — | RST_SENT in CQ-run | ✅ Shipped (v0.18.6) | — | — |
| — | Distance in miles (FT8) | ✅ Shipped (v0.18.6 + this session) | — | — |

---

## 🎯 v1.0 Release Gates (2 blockers for stable release)

The path to v1.0 is a complete standalone FT8 station with TX, logging, and ADIF upload.

### 1️⃣ LoTW Upload (TQSL)
- **Status:** [ ] Not yet designed
- **Blocker for:** v1.0.0 stable release
- **Challenge:** Certificate-based via TQSL (not simple HTTP API like QRZ/eQSL)
- **Reference:** See CLAUDE.md "Next up" + README.md Roadmap
- **Note:** QRZ (v0.16.2) and eQSL (v0.16.2) already shipped; LoTW is the last logging target. The mbedtls→PSRAM fix already unblocked outbound HTTPS, so the TLS issue QRZ hit won't recur — look elsewhere first if LoTW fails to connect.

### 2️⃣ FT8 TX Multi-day Soak
- **Status:** [ ] Testing needed
- **Blocker for:** v1.0.0 stable release + dropping "beta" label
- **Testing scope:**
  - Multi-hour/multi-day TX stability
  - No duty-cycle protection yet
  - No audio loopback verification
  - No over-temperature monitoring
- **Note:** All RX/panadapter/web/logging features already stable

---

## ✅ Closed Investigation

### FT8 decode-yield gap to v0.18.0 — CLOSED 2026-06-26
**Status as of v0.18.6:** Three real regressions found and fixed since v0.18.0, but a same-night A/B still showed v0.18.0 decoding noticeably better — left as an open question.

- Fix 1: `cw_audio_init()` (main.c) was spawning a priority-6 ghost task on core 1, preempting `fft_task` ~125×/slot — disabled.
- Fix 2: restored 3/4 of a separately-validated fix (`d140485`) that the v0.18.1 emergency revert had thrown out (CAT poll resilience, DiGi-forcing routing, freshest-freq snapshots).
- Fix 3: `ui_push_spectrum()` no longer does an unconditional LVGL opacity set every 10 Hz tick.

**Resolution:** ran a controlled same-time-of-day A/B (midday, sunny high-pressure, 20m) — built v0.18.0 in a separate git worktree, full chip erase before each flash to eliminate NVS/state carryover, two back-to-back 15-minute (60-slot) captures.

| | v0.18.0 | HEAD (all 3 fixes) |
|---|---|---|
| Mean decodes/slot | 15.38 | 15.38 |
| Std dev | 6.65 | 5.70 |
| First-half avg | 16.4 | 15.8 |
| Second-half avg | 14.3 | 15.0 |

Means identical to two decimal places; no decode-collapse cliff in either run. **Verdict: the v0.18.6 fix set fully closes the gap.** The earlier same-night A/B was comparing different few-minute windows as the band faded through the evening — a band-fading confound, not a code regression.

- **See:** `project_ft8_sparse_decode_investigation` in memory for full methodology + data.

---

## ✨ Feature Requests (from groups.io / Roy)

### One-button TUNE feature
**Requested by:** WS1M/bammi (Jun 25)

**Description:** Single button to trigger external relay/radio tuner function for shack use.

**Status:** Possibly already handled in groups.io message #172521 — needs verification if it's implemented or just discussed.

**Priority:** Medium

---

### CW page with memory support
**Requested by:** Someone in thread (Jun 24-25)

**Description:** Dedicated CW page (like the FT8 page) with ability to trigger/manage CW memory messages from the panadapter interface.

**Priority:** Medium

**Note:** Related to [[project_cw_audio_blocked]] — CW audio is currently shelved due to a priority/cadence issue in `cw_audio_task` (see Open Investigation above) plus the underlying I2S/DMA contention. Clarify scope: UI-only for manual CW memory triggering, or full CW demodulation/playback?

---

### FT4 mode
**Requested by:** Roy (Jun 26)

**Description:** Add FT4 alongside FT8.

**Priority:** Low–Medium

**Assessment:** Much lower effort than JS8/RTTY — the vendored `components/ft8_lib` already fully implements FT4 internally (`FTX_PROTOCOL_FT4` branches in `decode.c`; Costas pattern, 4-tone Gray map, symbol period, LDPC(174,91) all already in `ft8/constants.h`). Decode/encode core is essentially free. The real work is app-level slot-timing plumbing built around FT8's fixed 15 s slot: `ft8_test.c`'s capture/decode/TX loop, `ft8_qso.c`'s timeout-in-slots counters, the UI countdown bar, and CAT DigiMode forcing all need a parallel 7.5 s path. No new DSP pipeline or UI screen needed, unlike RTTY/JS8.

**See:** `project_ft4_mode_request.md` in memory, README.md "Longer term" roadmap.

---

## 🔧 Known Issues / Shelved Work

### CW Audio (shelved — v0.18.5, extended v0.18.6)
- **Status:** Shelved — disabled to restore FT8 decode performance
- **Root cause (v0.18.5):** `cw_audio_preopen()` (I2S/DMA init) and `dsp_cw_forward()` (hot-path call) degrade FT8 yield 2–3× even with CW audio off — disabled.
- **Root cause (v0.18.6, found later):** `cw_audio_init()` was never disabled alongside `cw_audio_preopen()` — it spawned a priority-6 task on core 1 that kept preempting `fft_task` every 120 ms for the whole session. Now also disabled, recovering ~25% of lost yield — but the gap to v0.18.0 is still open (see Open Investigation above).
- **Fix needed before re-enabling:** root-cause the original I2S/DMA/UAC contention, AND fix `cw_audio_task`'s priority/cadence so it can't preempt `fft_task` even when idle, then soak-test FT8 yield over a full session.
- **See:** `project_cw_audio_blocked.md` in memory system

---

## 🚀 Longer-Term Roadmap (Post v1.0)

### Audio & Monitoring
- **CW decoder** — Goertzel-based, text scrolling under spectrum. QMX already does this internally; question is mirror via CAT or parallel decoder on Tab5.
- **Tab5 speaker/headphone audio** — Demodulated CW/SSB passband audio from Tab5's own jack, so operator can monitor without QMX audio path.
- **Extended waterfall history** — PSRAM has room for several minutes of scrollback; two-finger drag to scrub through.

### Hardware & Modes
- **QMX (small) support** — Same UI, different USB endpoint config and band table.
- **JS8 / RTTY modes** — See feasibility docs in `docs/js8-feasibility.md` and `docs/rtty-feasibility.md`.
- **FT4 mode** — See Feature Requests above (Roy).

### UI & Performance
- **Phase 6.3 — Native-portrait rendering** (~50% FPS recovery)
  - Render directly in panel's native 720×1280 portrait coords (eliminate LVGL rotation step)
  - Significant UI rewrite; deferred due to effort
  - See [[project_phase63_status]] in memory system

### DSP & Signal Processing
- **DSP polish** — Noise reduction, auto-notch

---

## ✅ Shipped Since Last Update

### FT8 decode-yield gap to v0.18.0 (closed this session)
See "Closed Investigation" above — controlled A/B proved HEAD matches v0.18.0 exactly; the v0.18.6 fix set is sufficient.

### FT8 CQ-run RST_SENT bug (shipped v0.18.6)
Responder sending RR73/73 immediately never set `s_rst_sent`, so ADIF logged "599" instead of their actual SNR. Fixed in `ft8_qso.c` `cqrun_answer()`.

### FT8 distance-in-miles toggle (shipped v0.18.6 + this session)
- v0.18.6: added `distance_in_miles` setting, drawer checkbox, conversion in `ft8_screen_view.c`.
- This session: fixed two follow-on bugs — the checkbox was buried in the panadapter-only "Snap to signal" drawer section (now its own `DRAWER_SEC_DISTANCE` section, hidden in Panadapter mode, shown only in the FT8 drawer); the column header was hardcoded to "KM" and never flipped to "MI" (now updates live with the setting).

### Bottom-bar diag-log indicator polish (this session)
Red breathing dot moved net +30px right of the battery text; added a "Diag" text label next to it in the bottom bar's standard secondary text colour (not red).

---

## 📋 Completed in v0.18.x Series

| Version | Date | Items |
|---------|------|-------|
| v0.18.6 | Jun 26 | FT8 decode-yield investigation (cw_audio_init ghost task, d140485 restoration, opacity-set skip); RST_SENT fix; distance-in-miles; diag-log dot |
| v0.18.5 | Jun 25 | CW audio band-aid (preopen/dsp_cw_forward disabled); FT8 double-spawn crash guard restored; bootloader-corruption hotfix + recovery tooling |
| v0.18.4 | Jun 23 | Band-plan strip; one-finger pan/stroll; snap-to-signal toggle; band-aware worked-before; FT8 robot shelved |
| v0.18.3 | Jun 22 | Waterfall drawer controls (black level/contrast/floor blend/FFT window); display 180° flip |
| v0.18.2 | Jun 24 | WiFi idle reboot fix (SDIO RX mempool); web UI reconnect fixes |
| v0.18.1 | Jun 23 | Config backup/restore; clean-flash option; memory recall crash fix; fast toggle crash fix |
| v0.18.0 | Jun 22 | Streaming STFT decode; dual-core FT8; reply-on-immediate-slot; dBm scale restored; tap-to-tune fixes |

---

## 🎓 Reference Links

**Documentation:**
- `README.md` Roadmap section — Next up, Longer term
- `docs/version-history.md` — Full per-version changelog (v0.1.0 onward)
- `docs/js8-feasibility.md` / `docs/rtty-feasibility.md` — Mode feasibility studies
- `CLAUDE.md` "Branch state" + "Next up" — v1.0 gates, QRZ/eQSL details, CW Audio shelved analysis, FT8 decode-yield investigation

**Memory System:**
- `feedback_groups_io_proper_method.md` — How to extract text from groups.io threads
- `project_phase63_status.md` — Native-portrait UI rewrite (Phase 6.3)
- `project_cw_audio_blocked.md` — CW Audio shelved (I2S/DMA contention)
- `project_ft8_sparse_decode_investigation.md` — FT8 decode-yield investigation (closed 2026-06-26)
- `project_ft4_mode_request.md` — Roy's FT4 request + effort assessment

**Issue Tracking:**
- GitHub Issues / Discussions
- groups.io QRPLabs thread #119565643
