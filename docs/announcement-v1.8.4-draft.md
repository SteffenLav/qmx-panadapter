# v1.8.4 announcement — DRAFT for review (not posted)

Plain text for groups.io. Nothing below is markdown-formatted.

---

QMX Panadapter v1.8.4 is out.

The headline is that the QMX's own menu system is now on the Tab5's screen, and in the browser. Settings > Radio > "Radio menus" gives you the radio's 80x24 menu display with arrow keys, Enter and Back. For a QMX+ with no control panel this is the only way into its menus at all - Band config, System config, everything the front panel would reach. Randy N4OPI asked for it and Michael KZ4LY seconded it.

It runs on the radio's SECOND USB serial port, so the panadapter keeps decoding while you are in the menus. You have to switch that port on once, on the radio: System config > GPS & Ser. ports > USB serial ports > 2. Closing it walks the radio back out through its own "Exit terminal" item, and if you close the browser tab or leave it two minutes it hands the radio back by itself.

The rest of the release is fixes, and most of them came from you.

Auto-answer now switches itself off in the four places where it was transmitting when you would not expect it to (Roy KI0ER): it waits until it has heard both transmit windows before its first call, cancelling a transmission switches it off, a band change switches it off whichever way you changed band, and it is off at every startup. The band one mattered most - it only worked from the band buttons, so changing band from the browser, a spot, a memory recall or the radio's own knob left it running into an antenna that was probably not tuned.

A transmit offset you pick during a QSO is now used (Roy KI0ER). It was refused whenever a burst happened to be on the air, which is about four attempts in ten, and the exchange then carried on at the offset it started with - exactly when you were trying to get out from under someone.

Spur suppression now offers the setting that actually works first (Samuel W7STF). Both were always there, but the weaker one was offered first: measured on 20m, "Erase spur bins" takes the spur columns down about 78% on the waterfall against "Subtract"'s 28%. Samuel reported it as not seeming effective, and he was right - he was being given the weaker one.

A USB mouse is now read from its own description instead of an assumption (Kevin KW6E). A mouse reporting 16-bit movement had it read as 8-bit, so the pointer flew sideways and barely moved vertically.

And a WiFi hiccup can no longer restart the Tab5. The WiFi transport used to restart the whole device rather than drop a single frame, and because that was a clean restart there was no crash report - so it looked like a mystery reboot.

Smaller, all reported: the Close button in the QSO log is no longer red (it was a brighter red than "Delete all", which is backwards - Gyula HA3HZ); a QSO that could not be saved is no longer reported as logged, found while working out how much the log holds, which is about two thousand contacts (also Gyula); and the top bar no longer gets stuck showing the wrong mode or band.

One thing NOT fixed, and I would rather say so: the phantom CW stream - a mirror copy of a real signal, seen after using the radio's front-panel menu. Roy KI0ER can now reproduce it on demand, which is a big step, but the explanation I had for it was tested on the bench and disproved. So it is still open rather than quietly closed.

Download: the flasher zip is on the release page. Unzip it and run flash.bat on Windows or flash.command on macOS and Linux - nothing else needed.

https://github.com/SteffenLav/qmx-panadapter/releases/tag/v1.8.4

Full detail as always in docs/version-history.md, and the user guide is at tab5.lav.dk.

73 de Stef OZ1LAV
