============================================================
 QMX Panadapter - flashing the firmware onto an M5Stack Tab5
============================================================

This is a one-click flasher/updater. On Windows it downloads
everything it needs - the flashing tool (esptool) AND the latest
firmware - straight from GitHub, then flashes it. Nothing to
install, no developer tools, no Python. You just need internet
the first time.

(Offline? It still works if esptool and a firmware .bin are
already in this folder from a previous run - see "If it fails".)

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

 Install esptool once:   pip3 install esptool
    (or on macOS with Homebrew:  brew install esptool)

 Then:
    macOS - double-click  flash.command
    Linux - run           bash flash.command

 If the serial port is denied on Linux, add yourself to the
 dialout group once:   sudo usermod -aG dialout $USER
 (log out and back in afterwards).

 If macOS won't run it ("cannot be opened"), in Terminal:
    chmod +x flash.command       (once, then double-click again)

------------------------------------------------------------
 If it fails
------------------------------------------------------------

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
