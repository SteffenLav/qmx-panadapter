# TODO Assessment — Status of All Items (2026-06-25)

**Assessment Method:** Code grep + git log + memory system + prior context

---

## 🎯 v1.0 Release Gates

### 1️⃣ LoTW Upload (TQSL)
- **Status:** ❌ **NOT STARTED** — Genuinely new
- **Evidence:** No LoTW/TQSL code found; CLAUDE.md line 298 says "needs its own (harder) design before starting"
- **Blocker:** Design required first; this is not a code task yet
- **Assessment:** Genuine blocker item, not yet engaged

### 2️⃣ FT8 TX Multi-day Soak
- **Status:** ⚠️ **IN PROGRESS** — Features stable, testing pending
- **Evidence:** v0.18.x series shows stable FT8 TX (v0.18.0 added reply-on-immediate-slot, v0.18.2+ bug fixes)
- **Code maturity:** Fully functional, no obvious code defects
- **Assessment:** Code complete; needs operator soak testing (not a code task — validation work)

---

## 🐛 Bug Fixes

### #1: Fix FT8 CQ-run RST_SENT bug
- **Status:** 🔴 **IDENTIFIED, NOT FIXED** — Genuine bug
- **Evidence:** ft8_qso.c lines 475-478 confirmed
  ```c
  if (got_rr73 || got_73) {
      ok = send_next(FT8_TX_KIND_73, caller, our_freq, slot_sec, "73",
                     FT8_QSO_WAIT_DONE);
      // ↑ Never sets s_rst_sent! Falls through to adif default "599"
  } else {
      fmt_report(caller_snr, rpt, sizeof(rpt));
      ...
      strncpy(s_rst_sent, rpt, ...);  // ← Only here
  }
  ```
- **Impact:** ADIF logs record "599" instead of caller's SNR when they send RR73/73 immediately
- **Fix scope:** 5-10 lines; calculate SNR and set s_rst_sent in the if-branch
- **Assessment:** Ready to fix immediately

---

## ✨ Feature Requests (from groups.io)

### #2: One-button TUNE feature
- **Status:** ⚠️ **UNKNOWN — NEEDS VERIFICATION**
- **Evidence:** No TUNE code found; user said "already handled in #172521" (a prior groups.io message)
- **What we know:** #172521 was referenced in message history but not examined in this session
- **Assessment:** Verify before starting: grep the message thread for what was actually discussed

### #3: FT8 distance display in miles
- **Status:** ⏸️ **PARTIALLY EXISTED, NOW SHELVED** — New feature request but infrastructure exists
- **Evidence:**
  - `ft8_robot.c` line 28 has `grid_distance_km()` function (fully implemented)
  - Haversine distance calculation present
  - BUT: Robot feature is shelved/hard-disabled (line 68-75 in ft8_robot.c: `return;` before any logic)
  - Distance is never displayed in UI (`ft8_screen_view.c` has no distance field)
- **Why shelved:** Robot is WIP; greyed UI in v0.18.4, hard no-op in v0.18.5
- **Fix scope:** 
  - Unhide distance display in ft8_screen_view.c (new field)
  - Add miles toggle in settings drawer
  - Reuse `grid_distance_km()` but convert to miles
- **Assessment:** **Moderately new** — distance calc exists but UI display is new; robot infrastructure blocked

### #4: CW page with memory support
- **Status:** ❌ **NOT STARTED** — Genuinely new UI
- **Evidence:** No CW page/screen code found in ui/ directory; robot's code is shelved anyway
- **Related:** CW Audio itself is shelved (v0.18.5 band-aid — I2S/DMA contention)
- **Scope question:** Does this mean:
  - UI-only for manual CW memory keying (no audio demod)? → Feasible
  - Full CW demod + audio playback? → Blocked by shelved CW Audio
- **Assessment:** Genuinely new; scope needs clarification

---

## 🔧 Known Issues / Shelved Work

### CW Audio (v0.18.5 band-aid)
- **Status:** ✋ **DISABLED, SHELVED** — Known issue, not new
- **Evidence:** v0.18.5 commits show disabled `cw_audio_preopen()` and `dsp_cw_forward()` calls
- **Why:** I2S/DMA contention kills FT8 decode yield (39–45 → 11–15 msgs/slot even when CW off)
- **Root cause:** Pipeline redesign needed (async resampling or dedicated core for CW demod)
- **Assessment:** Documented shelved item; not a surprise

---

## 🚀 Longer-Term Roadmap (Post v1.0)

### Audio & Monitoring
#### CW decoder (Goertzel-based)
- **Status:** ❌ **NOT STARTED** — Genuinely new
- **Evidence:** No Goertzel code found; exists only in CLAUDE.md long-term description
- **Assessment:** New feature, not started

#### Tab5 speaker/headphone audio
- **Status:** ❌ **NOT STARTED (blocked)** — Depends on unblocking CW Audio
- **Evidence:** CW Audio shelved; this is the "passband audio out" version of it
- **Assessment:** Blocked by CW Audio shelving

#### Extended waterfall history
- **Status:** ❌ **NOT STARTED** — Genuinely new
- **Evidence:** Current waterfall is dual-height double-buffer (CLAUDE.md line ~414), no history scrollback
- **Assessment:** New feature, not started

### Hardware & Modes
#### QMX (small) support
- **Status:** ❌ **NOT STARTED** — Genuinely new
- **Evidence:** No QMX-small code; current codebase hardcoded for QMX+ (larger model)
- **Assessment:** New feature, not started

#### JS8 / RTTY modes
- **Status:** ❌ **NOT STARTED** — Feasibility docs don't exist yet
- **Evidence:** README.md mentions `docs/js8-feasibility.md` and `docs/rtty-feasibility.md` but they don't exist
- **Assessment:** New features; feasibility work not yet done

### UI & Performance
#### Phase 6.3 — Native-portrait rendering (~50% FPS recovery)
- **Status:** ⏸️ **DESIGNED, NOT IMPLEMENTED** — Infrastructure exists, significant rewrite pending
- **Evidence:**
  - `project_phase63_status.md` in memory describes exact layout + transform rules
  - `project_phase63_coordinate_transform.md` exists with coordinate math
  - No code changes in main branch yet
- **Scope:** Massive rewrite — `display.c`, `render_waterfall.c`, `ui.c` (all widgets), text rotation (2700°)
- **Assessment:** Planned but deferred; infrastructure documented, code work not started

### DSP & Signal Processing
#### DSP polish (noise reduction, auto-notch)
- **Status:** ❌ **NOT STARTED** — Genuinely new
- **Evidence:** No noise-reduction or notch-filter code; current DSP is DC blocker + IQ balance only
- **Assessment:** New feature, not started

---

## 📊 Summary Table

| Category | Item | Status | Effort | Notes |
|----------|------|--------|--------|-------|
| **v1.0 Gates** | LoTW (TQSL) | ❌ Not started | Design first | Blocker |
| | TX soak | ⚠️ Features stable | Testing only | Not code work |
| **Bugs** | RST_SENT | 🔴 Identified bug | 5–10 min | Ready to fix |
| **Groups.io** | TUNE button | ⚠️ Verify needed | ? | Check #172521 |
| | Distance/miles | ⏸️ Partial (robot code exists) | Medium | UI display new |
| | CW page | ❌ Not started | Medium–Large | Scope unclear |
| **Shelved** | CW Audio | ✋ Disabled | Unblock needed | I2S/DMA fix required |
| **Roadmap** | CW decoder | ❌ Not started | Large | New Goertzel impl |
| | Speaker audio | ❌ Not started (blocked) | Large | Needs CW Audio |
| | Waterfall history | ❌ Not started | Medium | New feature |
| | QMX small | ❌ Not started | Medium | New USB config |
| | JS8/RTTY | ❌ Not started | Large | Feasibility first |
| | Phase 6.3 | ⏸️ Designed, not coded | **Massive** | Layout doc exists |
| | DSP polish | ❌ Not started | Medium | New algorithms |

---

## 🎯 Recommended Action Plan

**Immediate (v0.18.6 quick fix):**
1. Fix RST_SENT bug (#1) — 5-10 min code, validates on next CQ-run session

**Next release (v0.19.0):**
1. Verify TUNE button status (check message #172521 in groups.io thread)
2. Add distance-in-miles display (reuse ft8_robot.c distance calc, UI only)
3. Clarify CW page scope (UI-only memory trigger vs full demod/audio)

**Later releases:**
- LoTW design (before v1.0)
- Long-session TX soak (validation, before v1.0)
- Extended roadmap (Phase 6.3, Goertzel, RTTY, etc.) — post v1.0

---

## ⚠️ Items NOT Ready to Code Yet

These need planning/design before code can start:
- **LoTW** — Design the certificate/TQSL integration first
- **CW page** — Clarify scope (UI-only vs full demod)
- **Phase 6.3** — Design is done, but rewrite is massive; needs sprint planning
- **JS8/RTTY** — Need feasibility docs first
