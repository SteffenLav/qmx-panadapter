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

## ⛔⛔ CRITICAL: THE QMX NEVER SURVIVES A FLASH

**Every flash with the radio attached wedges the QMX** (#74). It ALWAYS needs a manual power cycle by the operator afterwards. No exceptions. See [full rule](../../../memory/project_qmx_flash_wedge.md).

Standing rule:
1. Before flashing with radio attached: announce it needs a power cycle
2. After flashing: **ask** if they've power-cycled — never infer
3. If audio flows, they fixed it — don't credit the firmware

## ⛔ FOUR boards share this machine — resolve by BENCH NAME

Use `bench flash <name>`, `bench capture <name>`, `bench antenna <name>`. See [bench setup](../../../memory/project_bench_setup.md) for the registry and MAC addresses. All four boards enumerate as `VID_303A&PID_1001` with no serial number—the COM port follows the socket, not the board.

## Standing patches (MUST be applied before build)

11 patches in `tools/patches/apply_*.ps1`:
- `apply_esp_hosted_psram.ps1` — esp_hosted transport to PSRAM
- `apply_esp_hosted_sdio_recovery.ps1` — SDIO oversize RX recovery
- `apply_esp_hosted_rpc_orphan_resp.ps1` — RPC queue orphan deadlock
- `apply_fatfs_exfat.ps1` — exFAT support for large SD cards
- `apply_hcd_bulk_error_recovery.ps1` — USB bulk error tolerant
- `apply_hub_recover_tolerant.ps1` — USB hub recover tolerant
- `apply_cdc_acm_close_tolerant.ps1` — CDC-ACM close tolerant
- `apply_usb_dwc_hal_chan_error_tolerant.ps1` — USB DWC chan error tolerant
- `apply_hcd_buffer_parse_error_tolerant.ps1` — USB buffer parse error tolerant
- `apply_hcd_buffer_parse_no_urb_tolerant.ps1` — USB buffer parse no URB tolerant
- `apply_lvgl_port_task_psram.ps1` — taskLVGL stack to PSRAM

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
