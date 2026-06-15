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

 Install esptool once  (pip install esptool)  then run:

    esptool --chip esp32p4 -b 460800 write_flash 0x0 qmx_panadapter_merged_*.bin

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

   flash.bat                              (this repo)
   README.txt                             (this repo)
   esptool.exe                            (from the esptool release)
   qmx_panadapter_merged_vX.Y.Z.bin       (from the GitHub release)

 esptool.exe: download the Windows build from
   https://github.com/espressif/esptool/releases
   (esptool-vX.X.X-windows-amd64.zip -> copy esptool.exe here).

 flash.bat auto-detects the COM port and auto-picks the newest
 qmx_panadapter_merged_*.bin in the folder, so it needs no edits
 between releases - just drop in the new .bin.
