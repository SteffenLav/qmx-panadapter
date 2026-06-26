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
| 4 | **CW page** (Phase 1: TX/memory, Phase 2: RX decode) | ❌ Not started | P1 Medium, P2 Large/cheap-if-CAT | Ship P1 (page + canned-msg TX) standalone; gate P2 on Goertzel-vs-CAT-mirror question |
| 5 | FT4 mode (Roy) — *do before JS8/RTTY* | ❌ Not started | Low–Medium | ft8_lib already has FT4; generalizes the 15s slot-loop, which JS8 (10/15/30/60s variants) can then reuse |
| **Shelved** | | | | |
| 6 | CW Audio (speaker output only) | ✋ Shelved (v0.18.5/.6) | Unblock needed | Fix priority/cadence of cw_audio_task, then I2S/DMA contention. Blocks #4-P2-decoder only if it ends up needing the I2S path — does NOT block a CAT-mirror decoder, only #8 below |
| **Longer-Term Roadmap** | | | | |
| 7 | Speaker/headphone audio | ❌ Not started (blocked) | Large | Unblock CW Audio (#6) first |
| 8 | Extended waterfall history | ❌ Not started | Medium | New feature |
| 9 | QMX (small) support | ❌ Not started | Medium | New USB config |
| 10 | JS8/RTTY modes — *JS8 after FT4 (#5)* | ❌ Not started | Large | RTTY is a fully separate pipeline (feasibility doc); JS8 shares ft8_lib + benefits from FT4's slot-abstraction work |
| 11 | DSP polish (NR, notch) | ❌ Not started | Medium | New algorithms |
| **Closed/Shipped (since last update)** | | | | |
| — | FT8 decode-yield gap to v0.18.0 | ✅ Closed (v0.18.7) | — | Controlled A/B: HEAD == v0.18.0, 15.38 dec/slot each |
| — | FT8 auto-answer robot | ✅ Un-shelved, live TX (v0.18.7) | — | Permanent on-screen "unattended" disclaimer |
| — | CQ tone auto-relocation on clash | ✅ Shipped (v0.18.7) | — | Was just a warning before; now self-heals |
| — | SNTP/QMX time priority bug + FT8 auto-sync | ✅ Shipped (v0.18.7) | — | — |
| — | FT8 own-call highlight cache | ✅ Fixed (v0.18.7) | — | Was stale until a mode bounce |
| — | FT8 Filter modal checkbox sizing | ✅ Fixed (v0.18.7) | — | — |
| — | Recovery flasher port auto-detect | ✅ Fixed (v0.18.7) | — | Was hardcoded COM3 / wrong macOS glob |
| — | RST_SENT in CQ-run | ✅ Shipped (v0.18.6) | — | — |
| — | Distance in miles (FT8) | ✅ Feature in v0.18.6, bugs fixed in v0.18.7 | — | — |

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

### CW page (merged: TX/memory + RX decode, two phases)
**Requested by:** Someone in thread (Jun 24-25) — TX/memory half; the RX decode half was already on the longer-term roadmap separately and is folded in here since they're the two directions of the same feature.

**Description:** Dedicated CW page (like the FT8 page), with:
- **Phase 1 — TX/memory (medium effort, ship first):** canned-message buttons triggering CW TX, reusing the existing `mem_channels.c` + CAT key-down plumbing (same pattern as FT8's CQ presets). No new DSP, no audio dependency — can ship standalone.
- **Phase 2 — RX decode (effort depends on approach):** scrolling decoded text under the spectrum. Two options: (a) Goertzel-based tone tracker built from scratch (Large effort, new DSP code), or (b) mirror whatever the QMX itself already decodes internally via CAT (much cheaper — *check the CAT manual for a CW-decode query before committing to (a)*).

**Priority:** Medium

**Dependency correction:** Phase 2's decoder does **not** need the shelved CW Audio path (`cw_audio_task`/I2S output) — it only needs RX audio samples, which already flow through the same ring buffer FT8 decode taps. CW Audio (#6) blocks **Tab5 speaker/headphone output** (item below) only, not text decode. Don't gate Phase 2 on unblocking #6 unless option (a) turns out to need something CW Audio currently owns.

---

### FT4 mode — scope before JS8/RTTY, not in isolation
**Requested by:** Roy (Jun 26)

**Description:** Add FT4 alongside FT8.

**Priority:** Low–Medium

**Assessment:** Much lower effort than JS8/RTTY — the vendored `components/ft8_lib` already fully implements FT4 internally (`FTX_PROTOCOL_FT4` branches in `decode.c`; Costas pattern, 4-tone Gray map, symbol period, LDPC(174,91) all already in `ft8/constants.h`). Decode/encode core is essentially free. The real work is app-level slot-timing plumbing built around FT8's fixed 15 s slot: `ft8_test.c`'s capture/decode/TX loop, `ft8_qso.c`'s timeout-in-slots counters, the UI countdown bar, and CAT DigiMode forcing all need a parallel 7.5 s path. No new DSP pipeline or UI screen needed, unlike RTTY/JS8.

**Run together with JS8/RTTY planning:** FT4 forces generalizing the hardcoded-15s slot machinery into a mode-aware parameter. JS8 (which also heavily reuses `ft8_lib`, per the JS8 feasibility doc, and itself has 10/15/30/60s variants) directly benefits from that same generalization. RTTY doesn't — it's a fully separate pipeline (no LDPC, no block structure) per `docs/rtty-feasibility.md`, so it stays independent. **Recommendation:** do FT4 first as the cheap trial run of the slot abstraction, then revisit JS8 with that abstraction already in place; treat RTTY as unrelated.

**See:** `project_ft4_mode_request.md` in memory, README.md "Longer term" roadmap.

---

## 🔧 Known Issues / Shelved Work

### CW Audio — speaker/headphone output ONLY (shelved — v0.18.5, extended v0.18.6)
- **Status:** Shelved — disabled to restore FT8 decode performance
- **Scope correction:** this is the I2S-to-speaker output path only. It blocks "Tab5 speaker/headphone audio" below. It does **not** block a future CW text decoder (see "CW page" feature above) — decode only needs the RX audio ring buffer, which is unaffected.
- **Root cause (v0.18.5):** `cw_audio_preopen()` (I2S/DMA init) and `dsp_cw_forward()` (hot-path call) degrade FT8 yield 2–3× even with CW audio off — disabled.
- **Root cause (v0.18.6, found later):** `cw_audio_init()` was never disabled alongside `cw_audio_preopen()` — it spawned a priority-6 task on core 1 that kept preempting `fft_task` every 120 ms for the whole session. Now also disabled — and a controlled A/B on 2026-06-26 confirmed this fully restored yield to v0.18.0 levels (see Closed Investigation above).
- **Fix needed before re-enabling:** root-cause the original I2S/DMA/UAC contention, AND fix `cw_audio_task`'s priority/cadence so it can't preempt `fft_task` even when idle, then soak-test FT8 yield over a full session.
- **See:** `project_cw_audio_blocked.md` in memory system

---

## 🚀 Longer-Term Roadmap (Post v1.0)

### Audio & Monitoring
- **CW decoder (RX)** — now Phase 2 of the merged "CW page" feature request above, not a standalone item.
- **Tab5 speaker/headphone audio** — Demodulated CW/SSB passband audio from Tab5's own jack, so operator can monitor without QMX audio path. Blocked on unshelving CW Audio (#6) — see Known Issues above.
- **Extended waterfall history** — PSRAM has room for several minutes of scrollback; two-finger drag to scrub through.

### Hardware & Modes
- **QMX (small) support** — Same UI, different USB endpoint config and band table.
- **JS8 / RTTY modes** — See feasibility docs in `docs/js8-feasibility.md` and `docs/rtty-feasibility.md`. **JS8 benefits from doing FT4 first** (shared slot-abstraction work); RTTY is unrelated to either.
- **FT4 mode** — See Feature Requests above (Roy); do before JS8.

### DSP & Signal Processing
- **DSP polish** — Noise reduction, auto-notch

---

## ✅ Shipped Since Last Update

### FT8 decode-yield gap to v0.18.0 (closed in v0.18.7)
See "Closed Investigation" above — controlled A/B proved HEAD matches v0.18.0 exactly; the v0.18.6 fix set is sufficient.

### FT8 auto-answer robot un-shelved (v0.18.7)
Feature-complete since v0.18.4 but held back pending an on-air soak test that kept getting deferred. Shipped live rather than waiting indefinitely — the Filter modal's "Auto-answer CQ with priority:" row is un-greyed, `robot_en` persists, and a permanent (not one-time) "⚠ Transmits unattended — never leave running unsupervised" warning shows whenever the checkbox is checked.

### CQ tone auto-relocation on clash (v0.18.7)
`ft8_tx_is_clashing()` used to just show "⚠ FREQ BUSY" and keep transmitting on the occupied tone. `ft8_qso.c`'s new `relocate_cq_tone_if_clashing()` now re-scans and hops to the nearest clear slot on every CQ no-answer cycle. Active exchanges still keep the tone locked to the partner, unchanged.

### SNTP/QMX time priority bug + new FT8 auto-sync (v0.18.7)
`time_sync_notify_qmx()`'s 10-minute SNTP-freshness check didn't match SNTP's real ~hourly resync cadence, letting the QMX's non-GPS RTC silently win every 5-minute poll even with WiFi healthy. Now gated on `wifi_is_connected() && wifi_time_is_valid()`. Also new: the per-slot FT8 timing average auto-applies to the system clock every slot (damped, ~30% gain per slot to avoid chasing noise) instead of only via a manual modal tap.

### FT8 own-call highlight cache fix (v0.18.7)
Was only refreshed on FT8-mode entry, so setting your callsign via the CQ modal while already on the FT8 screen left the highlight dead until a mode bounce. Now refreshed every cycle in `rebuild_list()`.

### FT8 CQ-run RST_SENT bug (shipped v0.18.6)
Responder sending RR73/73 immediately never set `s_rst_sent`, so ADIF logged "599" instead of their actual SNR. Fixed in `ft8_qso.c` `cqrun_answer()`.

### FT8 distance-in-miles toggle (feature in v0.18.6, bugs fixed in v0.18.7)
- v0.18.6: added `distance_in_miles` setting, drawer checkbox, conversion in `ft8_screen_view.c`.
- v0.18.7: fixed two follow-on bugs — the checkbox was buried in the panadapter-only "Snap to signal" drawer section (now its own `DRAWER_SEC_DISTANCE` section, hidden in Panadapter mode, shown only in the FT8 drawer); the column header was hardcoded to "KM" and never flipped to "MI" (now updates live with the setting).

### FT8 Filter modal checkbox sizing + Priority dropdown restyle (v0.18.7)
Pixel-measured on hardware that giving a checkbox label text made its indicator ~30% bigger than a textless one. Fixed by making all 8 checkboxes textless with a separate label object, stacked in one left-aligned column. Priority dropdown got a darker background, matching text colour, and enough height that "Most distant" isn't clipped.

### Recovery flasher port auto-detection (v0.18.7)
`flash-recovery.bat`/`.command` hardcoded `COM3` / `/dev/cu.usbserial-*` — a field report (Samuel W7STF) hit this on a machine where the Tab5 was on COM12. Both now auto-detect the same way the main flashers do.

### Bottom-bar diag-log indicator polish (v0.18.7)
Red breathing dot moved net +30px right of the battery text; added a "Diag" text label next to it in the bottom bar's standard secondary text colour (not red).

---

## 📋 Completed in v0.18.x Series

| Version | Date | Items |
|---------|------|-------|
| v0.18.7 | Jun 26 | FT8 decode-yield gap CLOSED (controlled A/B); auto-answer robot un-shelved; CQ tone auto-relocation; SNTP/QMX time-priority fix + FT8 auto-sync; own-call highlight cache fix; Filter modal checkbox sizing; recovery-flasher port auto-detect |
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
- `project_cw_audio_blocked.md` — CW Audio shelved (I2S/DMA contention)
- `project_ft8_sparse_decode_investigation.md` — FT8 decode-yield investigation (closed 2026-06-26)
- `project_ft4_mode_request.md` — Roy's FT4 request + effort assessment

**Issue Tracking:**
- GitHub Issues / Discussions
- groups.io QRPLabs thread #119565643
