╔════════════════════════════════════════════════════════════════╗
║  QMX PANADAPTER - BOOTLOADER RECOVERY                         ║
║  For Tab5s flashed with faulty v0.18.5-hotfix                 ║
╚════════════════════════════════════════════════════════════════╝

YOUR TAB5 IS STUCK IN BOOTLOADER MODE
=====================================
If your Tab5 shows a black screen and won't boot after flashing
v0.18.5-hotfix, your bootloader was corrupted. This recovery tool
will fix it.

HOW TO RECOVER
==============
IMPORTANT: Choose the script for YOUR operating system:

WINDOWS USERS:
  1. Plug Tab5 into this PC with a USB-C DATA cable
     (it will power on automatically)
  2. Double-click:  flash-recovery.bat
  3. Wait for completion (3-5 minutes)
  4. Tab5 will reboot and show WiFi setup screen - normal!

MAC USERS:
  1. Plug Tab5 into this Mac with a USB-C DATA cable
     (it will power on automatically)
  2. Open Terminal and run:  bash flash-recovery.command
     (or right-click flash-recovery.command and select "Open")
  3. Wait for completion (3-5 minutes)
  4. Tab5 will reboot and show WiFi setup screen - normal!

⚠️  WARNING
===========
- This ERASES ALL data (WiFi passwords, settings, logs)
- You will re-enter WiFi credentials after recovery
- Do NOT unplug during the process

WHAT'S HAPPENING
================
The recovery script:
1. Erases the entire chip (removes the broken bootloader)
2. Writes the correct bootloader (0x2000)
3. Writes the correct partition table (0x8000)
4. Writes the correct application (0x10000)

This fix was needed because the faulty v0.18.5-hotfix wrote
firmware to the wrong address (0x00000000), overwriting the
bootloader.

QUESTIONS?
==========
GitHub: https://github.com/SteffenLav/qmx-panadapter/issues

