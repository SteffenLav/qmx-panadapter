# Getting a crash log from your Tab5

This lets us see *why* the panadapter is rebooting. It takes about 2 minutes
and doesn't need any developer tools installed.

## What you need

- The Tab5, connected to a Windows PC via USB-C (the same port/cable you
  used to flash it)
- The file [`capture_serial_log.ps1`](capture_serial_log.ps1) — download it
  from this page (or from the latest release page, "Assets")

## Steps

1. **Save** `capture_serial_log.ps1` to your Desktop (or any folder).

2. **Right-click** the file → **Run with PowerShell**.
   - If you see a blue "Windows protected your PC" popup, click
     **More info** → **Run anyway**.
   - If PowerShell says scripts are disabled, instead open PowerShell,
     `cd` to the folder where you saved the file, and run:
     ```
     powershell -ExecutionPolicy Bypass -File capture_serial_log.ps1
     ```

3. The script lists COM ports and asks which one to use. If only one is
   listed, it picks it automatically. Otherwise pick the one that mentions
   "USB Serial" or "JTAG".

4. **Power-cycle the Tab5** — unplug and replug the USB-C cable (or hold
   the power button to turn it off, then on again). Watch it boot, show
   the panadapter screen, then reset.

5. Let it cycle through **2–3 reboots** (about 30–60 seconds), then press
   **Ctrl+C** in the PowerShell window to stop.

6. A file named `qmx_log_<date>_<time>.txt` appears in the same folder.
   **Send that file back** — that's all we need.

## Notes

- The window will keep scrolling with text — that's normal, it's the
  device's debug output.
- If nothing appears at all, double-check the USB-C cable supports data
  (some cables are power-only) and try a different USB port.
