# QMX firmware 1_03_002 vs 1_04_002 — 1:1 CAT + feature comparison

**Purpose:** prepare the panadapter for QMX `1_04` compatibility and identify the new
capabilities worth utilising. This is TODO item **#22** (and unblocks **#3**, one-button TUNE).

**Sources** (pulled 2026-07-03):

| Source | Documents | Where |
|---|---|---|
| QMX CAT programming manual, fw **1_03_000** | the 1_03 baseline | `docs/qmx-reference/QMX_CAT_programming_manual_1.03.000.pdf` (local cache, gitignored) |
| QMX CAT programming manual, fw **1_04_001** | all 1_04 CAT changes (rev history: "Added new CAT commands and features for 1_04_001 (MD, MU, TR, RR, PS, AI commands)") | `docs/qmx-reference/QMX_CAT_manual_1.04.001.pdf` |
| QMX operating manual, fw **1_04_001** | non-CAT features | `docs/qmx-reference/QMX_operation_manual_1.04.001.pdf` |
| qrp-labs.com/qmx changelog | 1_04_000 (08-May-2026), 1_04_001 (12-Jun-2026), 1_04_002 (18-Jun-2026), **1_04_004**, **1_04_005 (06-Aug-2026)**, **1_04_006 (12-Aug-2026)** | web |

**⚠ Betas kept coming — checked again 2026-08-12.** This document was written against
`1_04_002`; the bench radio has been on **`1_04_004`** for a while and **`1_04_005`** and
**`1_04_006`** have since appeared. I only noticed because the operator saw the release
post, having assumed 004 was current — so check the changelog before concluding anything
is unfixed. Entries since 1_04_002 that touch us:

| Version | Entry | Why it matters here |
|---|---|---|
| **1_04_006** (12-Aug-2026) | "Bug fix: TX underlines the correct VFO indicator character now in B and Split mode" | Same area as the dual-VFO display that survives our split stand-down (see the report drafted for Stan KC7XE). **Our finding was measured on 1_04_004** — re-test on 006 before pursuing it. Not the same bug on its face: ours is the second frequency remaining on screen after split is cleared, not the underline |
| **1_04_006** | "Show S-meter in Digi mode" | Radio-side display only. Nothing for us |
| **1_04_006** | "Protection disabled when not in CW mode or when internal GPS (QLG3 in QMX+) selected" | The GPS/paddle-port protection that drops the radio into practice mode with a `G`. From 006 it applies only in CW **and** with the external GPS selected, so a QMX+ using its internal QLG3 is unaffected. Any user-facing note about practice mode while a GPS is connected is now version-dependent |
| **1_04_005** (06-Aug-2026) | "Bug fix: CAT `GP` command had reversed longitude sign" | We do not use `GP`. Noted so nobody re-derives it |
| **1_04_005** | first-dit spike, PA mod test, key-down current spike | Radio-side. Nothing for us |
| **1_04_001** | "Bug fix: CAT MU command and MM (when auto update enabled) loaded previously saved state (VFO etc)" | Directly relevant, and found late. "MM when auto update enabled" is **MM Effect = Immediate**. So `MU;` and MM-with-auto-reload mishandling saved VFO state is a KNOWN, already-fixed bug class. It also fits what we measured on 1_04_004: `MU;` discards session state (IQ mode `Q9` — the spectrum goes flat while USB audio keeps flowing at full rate) while an `MM` Set with MM Effect = Immediate does **not**. The two reload paths behave differently on purpose |

**Status of 1_04 (as of 2026-07-03, superseded above):** still **BETA**, no GA. Three betas so far.
`1_04_000` was QMX+-only (hung on QMX — fixed in `1_04_001`). `1_04_002` is one unified
image for QMX and QMX+, and **installing it executes a Factory Reset** (all radio settings
wiped; the user must re-select their firmware variant at the post-reset prompt).
`1_04_002` itself is only fixes on top of `1_04_001` (SWR-style EEPROM init, Virtual U3S
fixes, grid-locator display, repeated-CW-message truncation) — **no CAT changes between
1_04_001 and 1_04_002**, so the 1_04_001 CAT manual is authoritative for 1_04_002.

**Recommendation unchanged:** `1_03_002` remains the known-good firmware in our quick-start
until the items in §5 are verified on real 1_04 hardware.

---

## 1. Command-by-command CAT diff (manual 1_03_000 → 1_04_001)

### 1.1 New commands in 1_04

| Cmd | What it does | Relevance to us |
|---|---|---|
| **AI** | Auto-Info mode, now properly implemented: `AI0` off, `AI1` old format (auto `IF;` on any change **+ every 1.5 s**), `AI2` extended format (auto `IF;` on change only), `AI3` both | ⚠️ Biggest risk AND biggest opportunity. Risk: anything that enables AI makes the QMX emit **unsolicited `IF;` frames** that would interleave with our FA/MD/FW poll parsing — `process_cat_message()` has no `IF` handler today. Opportunity: `AI2` is an event-driven push of freq/mode/TX-state that could replace much of our 50 ms poll loop (§4.2). The 1_03 landmine ("never send `AI1;` — corrupts FA polling for the session") may or may not be fixed; **verify on hardware only** |
| **KD** | Get/Set key state: `KD1;` key-down, `KD0;` key-up, `KD;` reads | Optional: positive TX-keyed confirmation during FT8 bursts; remote CW keying. No current need — `TX;`/`RX;` + `TA` cover us |
| **MU** | Force reload of configuration parameters (activate pending `MM` Set writes) | Key to possibly simplifying the 3-write SSB-filter dance (§4.3) |
| **PS** | `PS0;` = complete power-off (clean shutdown); `PS;` always returns `PS1;` | Easy new feature: "Power off QMX" button in web UI / drawer (§4.4) |
| **TR** | Get/Set Tune Rate (knob step, `0`=10 MHz … `7`=10 Hz) | No current use; noted for completeness |
| **RR** | Get/Set RIT Rate (`4`=1 kHz … `8`=1 Hz) | No current use (we don't touch RIT) |

### 1.2 Changed commands

| Cmd | 1_03_000 manual | 1_04_001 manual | Impact on our code |
|---|---|---|---|
| **MD** | Set/Get: `3` (CW), `6` (FSK), `7` (CWR), `9` (FSR) *(manual predates SSB; real 1_03_002 also does 1/2 LSB/USB)* | Set/Get: `1` LSB, `2` USB, `3` CW, **`5` AM (new, 1_04_001)**, `6` FSK, `7` CWR, **`8` SWR Tune (new)**, `9` FSR. Note: **no `4` (FM)** — QMX has no FM | Our `kw_modes[]` table in `cat.c` already labels `5`→"AM" ✓. Digit `8` currently displays `"?"` — now shows `"TUNE"` (shipped). ⚠️ **How to *exit* Tune via CAT is NOT documented**: the manual's own Set list for `MD` is `1,2,3,5,6,7,8,9` — `0` never appears in it. The "`MD0;` exits" claim in earlier project notes traces to the original changelog summary, not the manual body — treat it as unverified until tested on hardware. The safe fallback is setting `MD` back to whatever digit was active before Tune was entered. ⚠️ **`MD;` Get lies while tuning**: after `MD8;` a Get returns the **prior** mode, not `8` (found on hardware 2026-07-03, confirmed by Stan KC7XE 2026-08-09 — "either a feature or a bug", the radio remembering where to return to). Digit `8` is therefore unreachable via the poll, and there is **no CAT signal for "the radio left Tune"** — `tune_modal.c` owns the session state itself |
| **PC** | "power output in tenths of a watt", e.g. `PC45;` = 4.5 W | Same manual text, but the changelog adds: **returns 3 digits when power ≥ 10 W** | Already handled — `cat_query_power_swr()`/`cat_pwr_swr_async_read()` were re-calibrated for the 3-digit case (see CLAUDE.md) ✓ |
| **MM** (semantics) | MM Set stores to EEPROM; when the new value takes effect is undocumented | New **"MM Effect"** config parameter (System config → CAT config): **"Immediate"** = every MM Set auto-reloads config and takes effect at once; **"On demand"** = takes effect only on menu enter/exit or on `MU;` | Directly relevant to the hard-won SSB-filter recipe (§4.3). Also `1_04_001` changelog: "CAT MU command and MM loaded previously saved state" bug was fixed |

### 1.3 Unchanged commands (manual text byte-identical apart from pagination)

`AG` `C2` `FA` `FB` `FR` `FT` `FW` `ID` `IF` `KS` `KY`(both variants) `LC` `ML` `OM` `PL`
`Q0`–`Q9` `QA` `QB` `QC` `QJ` `RC` `RD` `RG` `RT` `RU` `RX` `SA` `SM` `SP` `SS` `SW` `TA`
`TB` `TM` `TQ` `TX` `VN`

Notably for us: **`FA`, `FW`, `TM`, `TA`, `TX`, `RX`, `PC`-format, `SW`, `VN`, `ID` and `Q9`
are all textually unchanged** — the entire FT8 TX burst sequence and the poll loop are
spec-stable across the upgrade.

> ⚠️ `Q9` being "unchanged" in the manual does **not** match field evidence: Dirk DK7CVD's
> `1_04` unit silently failed to apply `Q9 1;` (IQ mode stayed off; radio's own System
> Config showed it disabled). Our v0.19.3/v0.19.4 mitigations (4× handshake retry,
> echo-aware readback, red on-screen banner) exist precisely for this. Root cause on the
> QMX side is unverified — top item for hardware testing (§5).

---

## 2. Our CAT surface, mapped against 1_04

Everything the panadapter sends/parses today, and whether 1_04 changes it:

| We use | Where | 1_04 status |
|---|---|---|
| `FA;` / `FA<11d>;` | poll + touch-to-tune (`cat.c`) | unchanged ✓ |
| `MD;` / `MD<d>;` | poll + mode set (`cat.c`) | range widened (5, 8 can now appear in responses; 4 never will) — table tolerant, `8` label cosmetic |
| `FW;` | poll (suppressed while SSB BW pinned) | unchanged; the *read-reasserts-stale-width* behaviour is undocumented in both manuals → re-verify on 1_04 (§5) |
| `VN;` | once at link-up | unchanged ✓ (will report `1_04_002`) |
| `ID;` | link-up | unchanged ✓ (`ID020;`) |
| `Q9 1;` / `Q9;` | IQ-mode enable + confirm (`link_task`) | manual unchanged; **field-broken on 1_04** — see §1.3 warning |
| `TM;` / `TM<hhmmss>;` | time sync (`time_sync.c`) | unchanged ✓ |
| `TX;` `TA<f>;` `TA0;` `RX;` | FT8/FT4 burst (`ft8_tx.c`) | unchanged ✓ |
| `PC;SW;` | post-burst power/SWR | PC 3-digit already handled ✓ |
| `MMSSB\|Filter RX=` / `MMSSB\|Bandwidth=` | SSB BW recipe (`cat.c`) | menu **paths** must be re-verified — 1_04 reorganised the CAT config submenu and added menu items; our paths are name-based (robust by design) but unproven on 1_04 (§5) |
| `MMCW\|CW passband=` / `MMCW\|CW offset;` | CW BW + pitch | same as above |
| `MMBand config.\|Band name (m)[i];` / `\|Frequency center[i];` | band table read | same as above |

**Never sent, still forbidden:** `AI1;` (1_03 landmine). On 1_04 this becomes a documented
feature but stays untouchable until hardware-verified.

---

## 3. Non-CAT 1_04 features (operating-manual level)

| Feature | What | Panadapter angle |
|---|---|---|
| **AM receive mode** (experimental, 150–3200 Hz filter) | new RX mode, MD digit 5 | UI already labels AM; snap grid already has an AM step. What `FW;` returns in AM is undocumented — check on hardware. Could add AM to our mode selector once verified |
| **SWR Tune mode** ("hold TX = TUNE", or `MD8;`) | radio transmits carrier, shows SWR | the enabler for TODO **#3** one-button TUNE (§4.1) |
| **Virtual U3S** (complete Ultimate3S QRSS/WSPR beacon inside the QMX) | standalone beacon mode | nothing to do — radio-native; panadapter would just be disconnected/idle. Don't try to CAT-poll while it beacons until tested |
| **Symmetric phase** SSB setting (recommend YES) + LSB TX distortion fix | TX audio quality | TX-side; worth an I/Q-image A/B on RX out of curiosity, nothing to code |
| **Supply protection** (SMPS safeguard), **SWR warn style**, **Modes enabled** config, fullscreen CW practice decode, LCD grid-edit UI, **PTT from DTR** | radio-native niceties | no panadapter surface. "Modes enabled" *could* hide modes from the radio's own menu — should not affect CAT `MD` set, but note it as a support-question source |

---

## 4. Opportunities — the "nice new features" ranked

### 4.1 One-button TUNE (TODO #3) — IMPLEMENTED 2026-07-03, pending hardware verification
Shipped as a dedicated drawer button (`DRAWER_SEC_TUNE` in `ui.c`), gated on
`cat_qmx_fw_at_least(1, 4, 0)` — invisible on `1_03_002`. Deliberately NOT folded into the
USB/LSB/CW/DiGi mode popup: Tune keys the radio continuously, so it gets the same
confirm-and-visible-ACTIVE-state treatment as FT8 TX/robot mode. Entering sends
`cat_request_mode("TUNE")` (→ `MD8;`) through the existing poll-task deferral; a live
SWR/power label polls `PC;SW;` via a new 4th poll-task phase
(`cat_tune_poll_set_active()`), active only during Tune. Exit restores the mode that was
active before Tune was entered (see §1.2 — NOT a bare `MD0;`, which the manual never
documents as valid), backed by a 60 s auto-timeout and continued `MD;` polling so an exit
via the radio's own front panel is also picked up. **Still needs real hardware to confirm:
does restoring the prior mode digit actually exit the radio's own Tune UI cleanly, and
what do `FW;`/other polls return while Tune is active?** — see the checklist in §5.

### 4.2 `AI2` event-driven state push — biggest architectural win, highest risk
`AI2` makes the QMX push an `IF;` frame on every freq/mode/TX-state change. That could
replace most of the 50 ms FA/MD round-robin: lower CDC traffic, faster UI response to
knob turns, and it shrinks the cross-thread-write race window that has cost us several
bugs. Needs: an `IF` response parser (fixed-format, documented §1.1), fallback to polling
if AI stops flowing, and proof that AI no longer corrupts the session like 1_03's `AI1;`
did. **Do not attempt any AI experiment on 1_03_002.**

### 4.3 Simplify the SSB-filter 3-write dance — deferred until proven
Hypothesis to test on 1_04: with "MM Effect = Immediate" (or an explicit `MU;` after the
MM Set), writing `MMSSB|Filter RX=` alone may now both persist *and* apply, and the
`FW;`-read-reverts-the-width bug may be gone (the `1_04_001` "MM loaded previously saved
state" fix smells related). If so, we can drop `s_ssb_bw_pinned` and the FW-poll
suppression — restoring live BW display while pinned. **Keep the current recipe until a
1_04 unit proves each step**; it must also keep working on 1_03_002 (runtime-gate any
simplification on `VN;`).

### 4.4 "Power off QMX" button — trivial once verified
`PS0;` is a clean remote shutdown. Nice for POTA (shut the radio down from the Tab5/web
UI before packing up). One deferred write via the poll task + a confirm dialog.

### 4.5 AM mode surfacing — IMPLEMENTED 2026-07-03, pending hardware verification
Added to both mode selectors (touch popup in `ui.c`, web dropdown in `index.html`), gated
the same way as Tune. The CAT-layer mapping already existed (`hamlib_mode_to_digit`,
digit 5) — the only real gap was that neither selector offered it. **Still unverified:
what `FW;` returns in AM** — if it's something unexpected, the passband-width label/overlay
could show garbage; no fixed passband overlay was added since the correct value isn't
known yet. Check on first real 1_04 contact.

**AM bandwidth is NOT selectable — do not add an AM BW menu (settled 2026-07-04, from the
manuals).** Field question: "why is AM fixed at 3.2 kHz, should there be other BWs?" Answer,
straight from the 1_04_001 manuals:
- `FW:` (CAT manual) is **get-only** — *"Returns 3200 in Digi mode, and 0300 for CW mode."*
  There is no `FW` Set. So the 3.2k the UI shows in AM is just the radio reporting the upper
  edge of its filter; it is not a value we (or the user) can change via `FW`.
- The "150–3200 Hz" figure is **one fixed filter, not a range**: the operation manual calls it
  the *"default 150-3200 Hz wide filter used for Digital modes"* (§CW Filters, the `None`
  option description). AM uses that same single wide audio filter.
- The QMX exposes selectable RX bandwidths **only** for SSB (2500/2700/2900/3200 via
  `MMSSB|Bandwidth`) and CW (the 54-filter set via `MMCW`). **Digi and AM have no filter
  selection at all** — no `MMAM|…` filter item exists, and the SSB filter menu is SSB-only.

Therefore `bw_popup_open()` in `ui.c` correctly returns without a menu for AM (and Digi) —
there is nothing to pick. The only *unverified* angle is whether writing `MMSSB|Bandwidth=`
while in AM would narrow the AM audio anyway; the manual gives no indication it does, so
don't add it speculatively — it'd need a bench test and might just return `?;`. The AM
passband *overlay* is drawn symmetric (±1600 Hz from FW=3200), which is cosmetically not
ideal for AM (really ±audio-BW around the carrier) — a display-only tweak if ever wanted,
unrelated to the (non-existent) BW selection.

Not worth pursuing now: `KD` (nothing our TX path lacks), `TR`/`RR` (knob ergonomics,
radio-side), `LC`/`TB` (LCD mirror / CW-decode text — cute, no demand).

---

## 5. Hardware verification checklist (blocks everything above)

Run against a real `1_04_002` unit, in this order — each item independent:

1. **Regression pass on the unchanged surface:** connect, IQ handshake, FA/MD/FW poll,
   touch-to-tune, FT8 RX decode, one FT8 TX burst into dummy load, `TM;` time sync,
   `PC;SW;` readout (at both <10 W and ≥10 W if the unit allows).
2. **`Q9 1;` IQ mode** — does Dirk's silent-failure reproduce? Does our 4× retry recover
   it, or does the red banner fire? Check the radio's own System Config → IQ Mode after.
3. **MM paths** — `MMSSB|Filter RX;`, `MMSSB|Bandwidth;`, `MMCW|CW passband;`,
   `MMCW|CW offset;`, `MMBand config.|Band name (m)[0];` all still resolve (no `?;`).
4. **`FW;` revert behaviour** — set a width, resume FW polling, see if it still snaps back.
   Then test "MM Effect = Immediate" + single `Filter RX` write (§4.3 hypothesis).
5. **`MD8;` Tune** — the "Antenna Tune" drawer button (implemented 2026-07-03) should
   appear automatically once `VN;` confirms `1_04`+. Tap it, confirm the radio actually
   enters Tune and transmits, confirm the live SWR/power label updates, then tap "Stop
   Tune" and confirm the radio returns cleanly to the mode it was in before (this is the
   real unknown — our code restores the prior mode digit rather than sending a bare
   `MD0;`, since the manual's Set list never lists 0; if that DOESN'T cleanly exit Tune on
   real hardware, this needs a different exit mechanism entirely). Also let the 60 s
   auto-timeout fire once on purpose to confirm it recovers cleanly. Separately confirm AM
   in the mode selector — Set works, and check what `FW;` reports once in AM.
   **Do this on a dummy load**, and note two things already settled by Stan KC7XE
   (2026-08-09) so they don't get re-tested here: CAT commands are safe while Tune is
   entered via `MD8;` (the lock-up is front-panel-menu only), and `MD;` will report the
   pre-Tune mode throughout — that is expected, not a failed entry. Judge "did it enter
   Tune" from the radio transmitting and the SWR/power readout, never from `MD;`.
6. **`AI` modes** — last, on an expendable session: `AI2;`, watch for unsolicited `IF;`
   frames, then `AI0;` and confirm normal polling still works. If anything corrupts,
   power-cycle and record it — that alone decides whether §4.2 is viable.
7. **`PS0;`** — clean shutdown works, radio comes back on a power cycle.

## 6. Safe-now code changes (harmless on 1_03_002)

- [x] `cat.c` `kw_modes[]`: digit `8` → `"TUNE"` (was `"?"`) — done 2026-07-03; keeps the
      top bar honest if a 1_04 user enters SWR Tune from the radio menu while connected.
- [x] Unsolicited `IF...;` frames: already handled — unmatched responses fall through
      `process_cat_message()` silently (no per-line warning), so an AI-enabled session
      wouldn't spam the diag log. Nothing to change.
- Everything else waits for hardware.

---

*Maintained alongside TODO #22. When 1_04 goes GA or a test unit becomes available,
work §5 top-to-bottom, then revisit §4.*
