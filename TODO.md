# QMX Panadapter — Master Todo List + Status Assessment

**Last updated:** 2026-06-25  
**Scope:** v1.0 release gates → next-up → longer-term roadmap  
**Source:** CLAUDE.md + README.md + groups.io feature requests (Jun 23-25)  
**Assessment:** Code grep + git log + memory system

---

## ⚡ Status Legend

- 🔴 **Identified bug** — Code defect confirmed, ready to fix
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
| **Bugs** | | | | |
| 3 | RST_SENT in CQ-run | 🔴 Identified | 5–10 min | Fix now |
| **Groups.io Features** | | | | |
| 4 | One-button TUNE | ⚠️ Verify needed | ? | Check message #172521 |
| 5 | Distance in miles | ⏸️ Partial | Medium | Reuse robot code, add UI |
| 6 | CW page w/ memory | ❌ Not started | Medium–Large | Clarify scope |
| **Shelved** | | | | |
| 7 | CW Audio | ✋ Shelved | Unblock needed | Fix I2S/DMA contention |
| **Longer-Term Roadmap** | | | | |
| 8 | CW decoder (Goertzel) | ❌ Not started | Large | New implementation |
| 9 | Speaker/headphone audio | ❌ Not started (blocked) | Large | Unblock CW Audio first |
| 10 | Extended waterfall history | ❌ Not started | Medium | New feature |
| 11 | Phase 6.3 native-portrait | ⏸️ Designed | **Massive** | Sprint-plan rewrite |
| 12 | QMX (small) support | ❌ Not started | Medium | New USB config |
| 13 | JS8/RTTY modes | ❌ Not started | Large | Feasibility docs first |
| 14 | DSP polish (NR, notch) | ❌ Not started | Medium | New algorithms |

---

## 🎯 v1.0 Release Gates (2 blockers for stable release)

The path to v1.0 is a complete standalone FT8 station with TX, logging, and ADIF upload.

### 1️⃣ LoTW Upload (TQSL)
- **Status:** [ ] Not yet designed
- **Blocker for:** v1.0.0 stable release
- **Challenge:** Certificate-based via TQSL (not simple HTTP API like QRZ/eQSL)
- **Reference:** See CLAUDE.md line 298 + README.md line 749
- **Note:** QRZ (v0.16.2) and eQSL (v0.16.2) already shipped; LoTW is the last logging target

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

## 📋 Next Up (After v1.0.0)

- **v1.0.0 stable release** — once LoTW designed + TX multi-day soak complete, polished UI, beta label gone

---

## 🐛 Bug Fixes (High Priority)

### #1: Fix FT8 CQ-run RST_SENT bug
**File:** `main/ft8_qso.c` — `cqrun_answer()` function (lines 475-478)

**Issue:** When a CQ responder sends RR73/73 immediately (without going through normal reply flow), `s_rst_sent` is never set, causing ADIF logs to default to "599" instead of using the caller's actual SNR.

**Root cause:** Code sends 73 but never calculates/sets `s_rst_sent` in that branch.

**Fix:** Calculate SNR from caller and set `s_rst_sent` before sending 73, matching the normal reply path logic (lines 485-486).

---

## ✨ Feature Requests (from groups.io, last 2-3 days)

### #2: One-button TUNE feature
**Requested by:** WS1M/bammi (Jun 25)

**Description:** Single button to trigger external relay/radio tuner function for shack use.

**Status:** You mentioned this was "already handled in #172521" — needs verification if it's implemented or just discussed.

**Priority:** Medium

---

### #3: FT8 distance display in miles
**Requested by:** BD4AHS (Jun 23)

**Description:** Add option/toggle to show FT8 station distance in miles instead of kilometers for imperial users.

**Priority:** Low (very minor request)

**Implementation:** Likely a simple toggle in settings + unit conversion in display logic.

---

### #4: CW page with memory support
**Requested by:** Someone in thread (Jun 24-25)

**Description:** Dedicated CW page (like the FT8 page) with ability to trigger/manage CW memory messages from the panadapter interface.

**Priority:** Medium

**Note:** Related to [[project_cw_audio_blocked]] — CW audio is currently shelved due to I2S/DMA contention killing FT8 decode performance. Clarify scope: UI-only for manual CW memory triggering, or full CW demodulation/playback?

---

## 🔧 Known Issues / Shelved Work

### CW Audio (v0.18.5)
- **Status:** Shelved — disabled to restore FT8 decode performance
- **Root cause:** I2S/DMA contention for internal DRAM competing with UAC stream
- **FT8 yield impact:** Drops from ~39–45 to ~11–15 msgs/slot even when CW is off
- **Fix needed:** Pipeline redesign (async resampling or dedicated core for CW demod)
- **See:** [[project_cw_audio_blocked]] in memory system

---

## 🚀 Longer-Term Roadmap (Post v1.0)

### Audio & Monitoring
- **CW decoder** — Goertzel-based, text scrolling under spectrum. QMX already does this internally; question is mirror via CAT or parallel decoder on Tab5.
- **Tab5 speaker/headphone audio** — Demodulated CW/SSB passband audio from Tab5's own jack, so operator can monitor without QMX audio path.
- **Extended waterfall history** — PSRAM has room for several minutes of scrollback; two-finger drag to scrub through.

### Hardware & Modes
- **QMX (small) support** — Same UI, different USB endpoint config and band table.
- **JS8 / RTTY modes** — See feasibility docs in `docs/js8-feasibility.md` and `docs/rtty-feasibility.md`.

### UI & Performance
- **Phase 6.3 — Native-portrait rendering** (~50% FPS recovery)
  - Render directly in panel's native 720×1280 portrait coords (eliminate LVGL rotation step)
  - Significant UI rewrite; deferred due to effort
  - See [[project_phase63_status]] in memory system

### DSP & Signal Processing
- **DSP polish** — Noise reduction, auto-notch

---

## 📋 Completed in v0.18.x Series

| Version | Date | Items |
|---------|------|-------|
| v0.18.5 | Jun 25 | FT8 double-spawn crash fix; bootloader recovery tool for affected users |
| v0.18.2 | Jun 24 | WiFi idle reboot fix (SDIO RX mempool); web UI reconnect fixes |
| v0.18.1 | Jun 23 | Config backup/restore; clean-flash option; memory recall crash fix; fast toggle crash fix |
| v0.18.0 | Jun 22 | Streaming STFT decode; dual-core FT8; reply-on-immediate-slot; dBm scale restored; tap-to-tune fixes |

---

## 🎓 Reference Links

**Documentation:**
- `README.md` line 741+ — Roadmap, Next up, Longer term
- `docs/version-history.md` — Full per-version changelog (v0.1.0 onward)
- `docs/js8-feasibility.md` / `docs/rtty-feasibility.md` — Mode feasibility studies
- `CLAUDE.md` line 294+ — v1.0 gates, QRZ/eQSL details, CW Audio shelved analysis

**Memory System:**
- `feedback_groups_io_proper_method.md` — How to extract text from groups.io threads
- `project_phase63_status.md` — Native-portrait UI rewrite (Phase 6.3)
- `project_cw_audio_blocked.md` — CW Audio shelved (I2S/DMA contention)

**Issue Tracking:**
- GitHub Issues / Discussions
- groups.io QRPLabs thread #119565643 (this thread)
