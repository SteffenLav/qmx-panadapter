# v1.8.5 announcement — draft for review

Post target: QRP Labs groups.io, thread *"QMX/QMX+ Panadapter for M5Stack Tab5"*

---

**QMX Panadapter v1.8.5 is out.**

https://github.com/SteffenLav/qmx-panadapter/releases/tag/v1.8.5

Most of this release is things I had already told people were fixed. Six of them
were finished after v1.8.4 was built, so if you read a reply from me and then went
looking for the fix, it genuinely was not in your build. That is on me, and it is
the main reason this release exists.

**Radio menus** — the terminal screen from v1.8.4 — got the work it needed to
actually be usable:

- A **cursor**, so you can see where you are typing.
- **BS deletes.** Land on a value, backspace away what you do not want, and type the
  rest. Randy N4OPI worked the editing model out from PuTTY and told me, and I still
  managed to ship the wrong byte first — 0x08 instead of 0x7F. His description was
  right all along; my translation of it was not. Thanks to Steffen for making me try
  it on the radio instead of believing myself.
- An **on-screen keyboard**, which matters if you do not have the snap-on Tab5
  keyboard: there was otherwise no way to enter a value at all.
- **"Exit terminal" no longer re-opens the session.** That was my bug, and Michael
  KZ4LY's guess at the cause was correct.
- If the radio has **no second serial port**, the full menu path is now on the
  screen — `System config → GPS & Ser. ports → USB serial ports → 2` — instead of a
  message that vanishes.

One thing worth knowing rather than treating as a fault: values longer than two
digits, and values inside a table, do not change with the arrows. They are
backspace-and-retype, and in a table the arrows move between columns. That is how the
radio behaves, not a Tab5 limitation.

**Other fixes, all from reports:**

- The clock **no longer claims `UTC(GPS)` on a radio with no GPS.** It was reading
  back a clock the Tab5 had set itself. Worse than a wrong label: once it believed
  in GPS it stopped maintaining the clock of the one radio that had no other source.
- **In CW the display follows the offset you actually set** (Roy KI0ER). It was read
  once when the Tab5 connected and never again, so changing it on the radio left
  tap-to-tune about 30 Hz off — and you transmitting off frequency.
- **The web log viewer can correct a report** (Gyula HA3HZ). It is called *View /
  edit log* and could only delete. Only the reports are editable, on purpose:
  callsign, band, mode, date and time are what QRZ, eQSL and LoTW match a contact
  on. And a report is now logged only if it was actually transmitted.
- **A caller who answers your CQ with a report instead of a grid** gets `R` plus your
  report, instead of another bare report (Gyula HA3HZ).
- **The station you are working never disappears from the decode list**, even with
  "Show only CQ callers" on (Roy KI0ER).
- **Leaving Radio menus hands the radio back properly** (Roy KI0ER) — no more pausing
  and resuming by hand to settle the waterfall.
- A **dated "Today only" ADIF export** (Gyula HA3HZ), and the **red transmitting
  banner no longer covers the text under it**.

**And one that had nothing to do with the feature list.** WiFi kept dying within a
few minutes of every boot on my bench, while the radio, audio and display carried on
perfectly. I blamed Bluetooth first — the log genuinely pointed there — and Steffen
disproved it in one step by switching Bluetooth off, after which it died anyway. The
real cause was three patches that had silently vanished from a part of the build tree
that is not in version control. The build now **refuses to compile** if any of them
is missing, naming the one to re-apply. If you have ever seen WiFi go quiet on a
build you compiled yourself, that was probably this.

Full detail, including what is *not* verified:
https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md

73 de OZ1LAV

---

## Notes for Steffen before posting

- The "Thanks to Steffen" line is written as if I am speaking. Reword or cut it —
  you are the one posting, so it may read oddly.
- Nothing in the release notes claims the unverified items are done. If anyone asks:
  Roy's phantom-waterfall cause is still unproven (the fix runs, and it covers both
  candidate causes), and the pileup reply gate has not been re-tested since I found
  a flaw in my own first version of it.
