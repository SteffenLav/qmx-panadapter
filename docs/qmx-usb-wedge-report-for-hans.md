# QMX USB enumeration wedge — report for Hans G0UPL

**Draft for the operator to review and send.** Supersedes the August 2026 draft, which said
"8 of 16 descriptor bytes" — that phrasing was misleading (see the note at the end).

Plain text below the line, ready to paste.

---

Hans,

I have a reproducible USB fault on the QMX/QMX+ that I have narrowed as far as I can from
the host side, and I think what is left is a firmware question. I run a panadapter on an
M5Stack Tab5 (ESP32-P4) that talks to the radio over USB — CDC-ACM for CAT and UAC for the
IQ audio.

**The symptom.** After the Tab5 is reset while the radio is attached and streaming — a
reflash, or any warm restart — the QMX will sometimes no longer enumerate. Not
intermittently slow: it never comes back until the QMX itself is power-cycled. A cold boot
of the Tab5, with the radio already attached, enumerates perfectly. It is specifically the
case where the host disappears mid-session.

**What the radio does.** On every enumeration attempt the host sends the standard first
request:

    GET_DESCRIPTOR (DEVICE), address 0, wLength 8

The setup packet is acknowledged and then **zero data bytes come back** — an empty data
stage. The host reports it as "device response length 8, expected 16" because it counts its
own 8-byte setup packet in both figures. The radio is visible on the bus (the host detects
the connection and retries enumeration repeatedly) but never returns a descriptor byte.

**Reproduced on 1_03_002 and 1_04_004**, and I could see nothing USB-related in any 1_04
changelog.

**What I have ruled out on the host side.** All of these were tested against a radio in the
wedged state, and every one produced the identical empty data stage:

- reset hold of 30 ms, 50 ms and 200 ms (the last is what Linux uses for a stubborn device),
  with reset recovery up to 100 ms
- requesting 64 descriptor bytes instead of 8, which is what Windows and Linux ask for — the
  radio returns nothing to either
- root-port power cycling, repeatedly
- removing VBUS from the connector for 2 s and for 8 s
- closing the interfaces properly first — CAT sent `TA0;` and `RX;`, the CDC interface
  closed, the audio interface set to alternate setting 0 — and only then dropping the port
- the same orderly close followed by removing VBUS and holding it off through the reset and
  the entire host boot (several seconds) before restoring it, which is as close to a physical
  unplug as the board can produce
- physical cable replugs, in earlier sessions

Only a QMX power cycle — a firmware restart — clears it. VBUS can keep the STM32 alive, so
it does appear to be the firmware restart that matters rather than the power itself.

**What I think this means, and where I could easily be wrong.** It looks as though the
radio's USB stack, when the host vanishes mid-session, ends up in a state where it no longer
responds at address 0 and does not return to the default state on a bus reset. If that is
right, the interesting question is whether the peripheral's reset handling is reachable from
whatever state it lands in.

I am not asking you to debug my host. If this is a known behaviour, or if there is something
the host should be doing that I am not, I would be glad to hear it — and if it is useful I
can capture anything you would like from this side.

73
de Steffen OZ1LAV

---

## Notes for the operator (not part of the message)

- **The "8 of 16 bytes" phrasing in the August draft was wrong** and would have sent Hans
  looking for a truncated descriptor. Both figures include the host's own 8-byte setup
  packet, so "actual 8" means **zero descriptor bytes** — the radio sent nothing. The
  message above says that plainly.
- The claim "survives physical cable replugs" comes from earlier sessions, not from
  2026-08-11. Worth re-confirming with a deliberate replug before sending, since it is the
  one line Hans is most likely to push back on — it is also the line that makes this a
  firmware question rather than a host question.
- Everything else in the list was measured on 2026-08-11 and is in TODO #74 and CLAUDE.md.
