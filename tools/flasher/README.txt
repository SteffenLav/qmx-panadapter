============================================================
 QMX Panadapter - flashing the firmware onto an M5Stack Tab5
============================================================

This folder is a self-contained, offline flasher. No developer
tools, no Python, no internet needed.

------------------------------------------------------------
 WINDOWS - the easy way
------------------------------------------------------------

 1. Plug the Tab5 into your PC with a USB-C cable that carries
    DATA (a charge-only cable will not work).

 2. Double-click  flash.bat

 3. Follow the prompt (press a key), wait for "SUCCESS", done.
    The Tab5 restarts on the new firmware automatically.

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

------------------------------------------------------------
 FOR THE MAINTAINER (Steffen) - what to put in this folder
------------------------------------------------------------

 The release ZIP should contain, all in one folder:

   flash.bat                              (this repo, Windows)
   flash.command                          (this repo, macOS/Linux)
   README.txt                             (this repo)
   esptool.exe                            (from the esptool release; Windows only)
   qmx_panadapter_merged_vX.Y.Z.bin       (from the GitHub release)

 Windows users need nothing installed (esptool.exe is bundled).
 macOS/Linux users install esptool once via pip3/brew (no standalone
 binary to bundle there).

 esptool.exe: download the Windows build from
   https://github.com/espressif/esptool/releases
   (esptool-vX.X.X-windows-amd64.zip -> copy esptool.exe here).

 flash.bat auto-detects the COM port and auto-picks the newest
 qmx_panadapter_merged_*.bin in the folder, so it needs no edits
 between releases - just drop in the new .bin.
