============================================================
 QMX Panadapter - flashing the firmware onto an M5Stack Tab5
============================================================

This is a one-click flasher/updater. THE FIRMWARE IS ALREADY IN
THIS FOLDER - the four .bin files next to these scripts are what
gets flashed, and nothing is fetched to get them. On Windows the
only thing downloaded is the flashing tool itself (esptool), once,
straight from Espressif's GitHub. Nothing to install, no developer
tools, no Python.

(Offline? It works with no internet at all once esptool is in the
"esptool" subfolder from a previous run - see "If it fails".)

------------------------------------------------------------
 v1.15.0 - this is the release that needs the cable
------------------------------------------------------------

 v1.15.0 rewrites the partition table to give the firmware room
 to grow, and an over-the-air update cannot do that - it can only
 ever write the app, never the map of the flash. So this one
 arrives as a flasher download. It is the only release that needs
 it; everything after it updates over WiFi again as before.

 If you tapped the update notice on the Tab5 and it said the
 download could not be reached, that was not a fault - there is
 deliberately no over-the-air image for this release.

 YOUR SETTINGS, MEMORY CHANNELS, QSO LOG AND LoTW CERTIFICATE ARE
 KEPT. Press ENTER at the flash-type prompt. Do NOT press E -
 that erases the whole chip, including your log and your LoTW
 private key, and it is not needed here.

------------------------------------------------------------
 WINDOWS - the easy way
------------------------------------------------------------

 1. Plug the Tab5 into your PC with a USB-C cable that carries
    DATA (a charge-only cable will not work).

 2. Double-click  flash.bat

 3. On the first run it downloads esptool and the latest firmware
    (a few seconds), then asks you to press a key. Wait for
    "SUCCESS", done - the Tab5 restarts on the new firmware.
    (esptool is cached in an "esptool" subfolder, so later runs
    only download the firmware.)

 That's it. Your saved settings (WiFi, callsign, grid, memories)
 are kept - this flasher does not erase them.

------------------------------------------------------------
 Mac / Linux
------------------------------------------------------------

 You need esptool installed once:

    macOS - brew install esptool      (recommended)
    Linux - pip3 install esptool

 NOTE for macOS: "pip3 install esptool" often FAILS on recent
 macOS with an "externally-managed-environment" error, even with
 Python installed. If that happens, use Homebrew instead
 (brew install esptool) or:  pipx install esptool

 Then run the flasher:
    macOS - double-click  flash.command
    Linux - run           bash flash.command

 The simplest, trouble-free way on a Mac is to open Terminal in
 this folder and run:   bash flash.command
 (That needs no "chmod" and sidesteps the security prompt below.)

 macOS security prompt: the first time, macOS may block the script
 ("unidentified developer" / "cannot be opened"). Either:
    - Right-click flash.command -> Open -> Open  (trusts it once), or
    - System Settings -> Privacy & Security -> "Open Anyway".

 "permission denied"? The download stripped the run permission.
 Just use   bash flash.command   (no fix needed), or once run:
    chmod +x flash.command       (then double-click again)

 If the serial port is denied on Linux, add yourself to the
 dialout group once:   sudo usermod -aG dialout $USER
 (log out and back in afterwards).

------------------------------------------------------------
 Normal flash vs CLEAN flash (erase all settings)
------------------------------------------------------------

 Both flashers now ask, just before flashing:

    "Type E for a CLEAN/ERASE flash, or just Enter for a
     normal flash"

 - NORMAL (just press Enter): updates the firmware and KEEPS
   all your saved settings (WiFi, callsign, grid, memories).
   This is what you want almost every time.

 - CLEAN (type E, then Enter): wipes the whole chip first, so
   *** ALL saved settings are PERMANENTLY ERASED ***:
     - your WiFi network name AND password
     - your callsign and Maidenhead grid
     - ALL saved memory channels
     - your logged QSOs (the ADIF log)
   You will have to re-enter everything afterwards, and any
   un-uploaded QSOs are gone. This cannot be undone.

 Only use CLEAN if something is stuck or corrupted - for
 example if WiFi refuses to turn on no matter what you do, or
 settings behave oddly. The clean flash clears the stored
 settings so the radio starts fresh. (A clean flash also takes
 a little longer, because it erases the entire chip first.)

------------------------------------------------------------
 If it fails
------------------------------------------------------------

 - REBOOT THE TAB5 AND TRY AGAIN. Do this first if the flasher can
   see the COM port but the flash keeps failing - the port still
   listed in Windows Device Manager, esptool still refusing. The
   Tab5's USB serial connection is produced by the ESP32-P4 itself,
   not by a separate chip, so a busy or unhappy running firmware can
   leave the port visible while it no longer answers. A reboot clears
   it. Reported by Samuel W7STF after a 7-hour session, and it worked
   immediately for him.
 - "FLASH FAILED" almost always means a charge-only USB-C cable.
   Try a different cable - it must carry data.
 - Close any serial monitor / Arduino IDE / other app that might
   be holding the COM port, then try again.
 - Unplug, replug, and re-run.

 No internet (offline flashing):
 - The FIRST run on Windows needs internet to fetch esptool. After
   that, esptool is cached in the "esptool" subfolder and reused.
 - For the firmware: the flasher tries GitHub first; if it can't
   reach it, it falls back to a qmx_panadapter_merged_*.bin sitting
   in this folder.
 - So to flash fully offline, run it online once (to cache esptool),
   then keep a qmx_panadapter_merged_*.bin next to it - both will be
   used automatically with no internet.
