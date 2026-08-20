# v1.8.8 — 2026-08-20

A diagnostics and field-report release. Nothing here changes how the radio is
operated; several things change how much can be found out when something goes
wrong.

## A crash now survives the reboot

Until now a crash left **nothing** on the device. If your Tab5 rebooted and you
sent a diagnostic download, it contained everything except the one thing needed —
the panic text went straight out of the serial port and was gone.

The next boot now reports the previous crash in the log, with the reason, the
assert text, **which task died**, how far into the run, the registers, and a
ready-made decode command. It reaches both **Diagnostic download** files.

So if the Tab5 restarts unexpectedly, please just send the diagnostic download.
That is now enough.

A side effect worth knowing: a reset with no crash record is positive evidence
that it was **not** a crash — a power cut or a reflash looks identical otherwise.

## FT8: the double-spawn root cause, open since v1.3.0

There has been one unreproducible reboot on entering FT8 for a long time. It now
has a cause: an internal "is the FT8 task running" flag was set by the task
itself rather than when it was created, and these tasks run at the lowest
priority on the board — so a watchdog could look during the gap, conclude nothing
was running, and start a **second** one. Both then built the same buffers and the
second freed memory the first was still using.

Fixed, along with a second instance of the same mistake found by checking the
whole codebase for it.

## FT8 decodes were being quietly discarded

Measured over a 54,142-decode run: **99 decodes thrown away** by an internal
timeout that was far too short for what was at stake. A dropped decode is gone —
it never reaches the list, the occupancy map, or the check for a reply addressed
to you. A lost reply looks exactly like the other station going quiet.

## From Samuel W7STF

- **Bandwidth stayed on a CW filter after switching CW → LSB.** Real bug: once an
  SSB filter had been pinned, nothing ever repainted the label on the way back
  into SSB. Fixed and verified on the radio.
- **The out-of-band tuner now exists on the web page too.** Out of band the Tab5
  turns the band strip into a centre-detented coarse tuner; the browser simply
  hid it — the one place you most want a way back to a band had no control at
  all. Same behaviour as the Tab5: drag off centre, springs back on release, full
  deflection is half the visible span.
- **Web stalls.** A real cause was found, and it is not your PC. The message that
  tells a displaced browser "another browser took the live view" was a malformed
  websocket frame, so browsers failed the connection instead of reading it,
  reconnected, and took the view straight back — a continuous tug-of-war whenever
  a second browser or a phone was left open. Measured before and after: **16
  takeovers in ten seconds → 2 in twenty-five**.
- **Radio Menus / Diagnostics colour.** Not fixed yet, deliberately. The colours
  the *menu* screens use are all handled; the Diagnostics screen evidently sends
  something else, and rather than guess, the firmware now reports exactly which
  codes it did not understand. If you can open Diagnostics and send the reading,
  that is the fix.

## Also

- The frequency readout could get stuck showing an old frequency while the
  spectrum, waterfall and radio were all correct. It was a dropped screen update
  that never retried; the poll now re-asserts it.
- Web page shows websocket health (`sessions`, `takeovers`, `closes`,
  `partial`) in `/api/status`, so a reported stall can be matched against what
  the device actually did instead of argued about.
- An internal audit of a class of start-up crash: two real gaps found and fixed,
  three other places verified genuinely safe.

## Not changed

No change to FT8/FT4 operation, CAT behaviour, logging, uploads or the manual.
