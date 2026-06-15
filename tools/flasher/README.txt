============================================================
 QMX Panadapter - flashing the firmware onto an M5Stack Tab5
============================================================

This is a one-click flasher/updater. It downloads the LATEST
firmware from GitHub automatically and flashes it - so you don't
have to find or download the right .bin yourself. No developer
tools and no Python needed on Windows.

(Offline? It also works without internet if a firmware .bin is
sitting in this same folder - see "If it fails" below.)

------------------------------------------------------------
 WINDOWS - the easy way
------------------------------------------------------------

 1. Plug the Tab5 into your PC with a USB-C cable that carries
    DATA (a charge-only cable will not work).

 2. Double-click  flash.bat

 3. It fetches the latest firmware, then asks you to press a key.
    Wait for "SUCCESS", done - the Tab5 restarts on the new
    firmware automatically.

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
 - The flasher tries GitHub first. If it can't reach it, it falls
   back to a qmx_panadapter_merged_*.bin in this same folder.
 - So for offline use, just download that .bin once (from the
   GitHub Releases page) and keep it next to flash.bat /
   flash.command. It will be used automatically when offline.

------------------------------------------------------------
 FOR THE MAINTAINER (Steffen) - what to put in this folder
------------------------------------------------------------

 The Windows ZIP should contain, all in one folder:

   flash.bat                              (this repo, Windows)
   flash.command                          (this repo, macOS/Linux)
   README.txt                             (this repo)
   esptool.exe                            (from the esptool release; Windows only)

 The firmware .bin is NOT bundled - the flasher downloads the
 latest release from GitHub itself. (Add a
 qmx_panadapter_merged_*.bin to the folder only if you want an
 offline-capable bundle; it's used as the fallback when there's
 no internet.)

 Windows users need nothing installed (esptool.exe is bundled).
 macOS/Linux users install esptool once via pip3/brew (no standalone
 binary to bundle there).

 esptool.exe: download the Windows build from
   https://github.com/espressif/esptool/releases
   (esptool-vX.X.X-windows-amd64.zip -> copy esptool.exe here).

 The repo hardcoded for downloads is SteffenLav/qmx-panadapter
 (REPO= in flash.bat / flash.command). No edits needed between
 releases - it always pulls the newest published release.
