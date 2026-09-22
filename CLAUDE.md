# CLAUDE.md — QMX Panadapter

ESP-IDF firmware for M5Stack Tab5 (ESP32-P4). Real-time panadapter for the QRP Labs QMX/QMX+ HF transceiver: USB host captures IQ audio (UAC) + CAT (CDC-ACM), FFT runs on-device, spectrum + waterfall rendered on the 5" touch display (landscape 1280×720).

**Detailed project truth has been moved to the memory system** (C:/Users/Steffen/.claude/projects/C--dev-qmx-panadapter/memory/) to keep sessions fast. This file contains only build essentials and the most critical operational rules.

## Build & flash

```powershell
idf.py build flash monitor          # standard IDF
qmx bfm                             # PowerShell helper (build + flash + monitor)
qmx fm                              # flash + monitor (skip rebuild)
qmx m                               # monitor only
```

Exit monitor: `Ctrl+T` then `Ctrl+X`.

## ⛔⛔ NEVER START A RELEASE UNLESS HE SAYS "WRAP-UP"

**The release process begins on the word "wrap-up" and on nothing else.** Not on
"I am eager to release", not on "the replies are sent", not on a list of fixes
looking finished, and not because it seems obviously next.

Touching README's version banner, creating a tag, building the PDF or the site —
any of it — IS starting a release. Do not.

⚠ 2026-09-22: he said he was eager to ship a "resolving" release and I began
editing the README banner on my own. Wrong: eagerness is context, not the
instruction. Ask, or wait for the word.

**What to do instead:** say what is ready and what is unverified, and stop.

## ⛔⛔ CRITICAL: THE QMX NEVER SURVIVES A FLASH

**Every flash with the radio attached wedges the QMX** (#74). It ALWAYS needs a manual power cycle by the operator afterwards. No exceptions. See [full rule](../../../memory/project_qmx_flash_wedge.md).

Standing rule:
1. Before flashing with radio attached: announce it needs a power cycle
2. After flashing: **ask** if they've power-cycled — never infer
3. If audio flows, they fixed it — don't credit the firmware

## ⛔ FOUR boards share this machine — resolve by BENCH NAME

Use `bench flash <name>`, `bench capture <name>`, `bench antenna <name>`. See [bench setup](../../../memory/project_bench_setup.md) for the registry and MAC addresses. All four boards enumerate as `VID_303A&PID_1001` with no serial number—the COM port follows the socket, not the board.

## Standing patches (MUST be applied before build)

⛔ **NEVER enumerate them here — run the checker.** This section said "11 patches"
while 23 `apply_*.ps1` scripts existed (24 patched sites). The same rot was recorded
at v1.10.1, when it said three and there were four. A count in prose cannot keep up.

```powershell
Get-ChildItem tools/patches/apply_*.ps1 | ForEach-Object { powershell -File $_.FullName }
python tools/check_patches.py     # the authority - refuses the build if any is missing
```

They live in git-ignored `managed_components/`, so `idf.py fullclean`, a dependency
refresh, or the release's `rm -r managed_components/` wipes them. All are idempotent.

⚠ **A release is the first time a component upgrade can break a patch**, because only
the release re-fetches. At v1.16.0 `apply_cdc_acm_close_tolerant.ps1` stopped matching
upstream and the build was correctly refused — see
[patch went stale](../../../memory/project_standing_patch_went_stale_cdc_acm.md).
When a clean build fails, **read `build/log/idf_py_stdout_output_*`**, not idf.py's
summary: it prints an unrelated `PRI` format-specifier HINT that reads like the cause.

See [patches detail](../../../memory/project_standing_patches.md).

## Serial capture rules

**Capture is the diagnostic record** — see [full serial capture rules](../../../memory/project_serial_capture.md). Quick version:

1. Use `bench capture <name>` (scheduled task, survives app updates)
2. Never stop and restart in the same PowerShell call
3. A stale file + process still running = dead capture (check content, not timestamps)
4. `bench standdown <name>` stops capture + clears watchdog

## Project documentation

Detailed sections moved to memory for session speed:
- [Module map](../../../memory/project_module_map.md) — main/ directory structure
- [Critical quirks](../../../memory/project_critical_quirks.md) — hardware, firmware hazards
- [Display layout](../../../memory/project_display_layout.md) — 1280×720 panels
- [CAT protocol](../../../memory/project_cat_protocol.md) — QMX commands
- [RTC and time sync](../../../memory/project_rtc_time_sync.md)
- [Bench setup](../../../memory/project_bench_setup.md) — MAC, COM, antenna
- [Diagnostic logging](../../../memory/project_diagnostic_logging.md)
- [FT8/FT4 features](../../../memory/project_ft8_features.md)
- [Release process](../../../memory/project_release_process.md)

See MEMORY.md index for complete topic list.

## Context management

To keep sessions fast: CLAUDE.md stays under 100 lines, detailed sections live in memory. Search MEMORY.md for topics before digging into git history or old files.
