# Draft groups.io announcement — v1.4.0

For the operator to review and post to the QRP Labs groups.io thread
"QMX/QMX+ Panadapter for M5Stack Tab5". Nothing posts this automatically.

---

**Subject:** QMX Panadapter v1.4.0 — live spots on the spectrum, and three instability causes root-caused

v1.4.0 is out: https://github.com/SteffenLav/qmx-panadapter/releases/tag/v1.4.0

Download the flasher zip, unzip, run flash.bat (Windows) or flash.command (Mac/Linux). A normal flash keeps your settings, log and certificates.

**Live spots on the spectrum.** POTA activations are now drawn straight onto the trace, at the frequency the station is actually using — callsign with a thin line down to the frequency scale. Grey means you have already worked that station on that band, so you can see at a glance who you still need. Press and drag across the callsigns and lift your finger to pick one: the Tab5 tunes to it and sets the mode. Spots fade with age and vanish after 30 minutes. Counts in the corners tell you how many more are just outside the window, and tapping one takes you there. RBN (the CW skimmer network) can be switched on as a second source — off by default, because unlike POTA it is a continuous feed.

**Three long-standing problems were root-caused this week, not worked around.**

- If your QRZ, eQSL or LoTW uploads have been failing, this release very likely fixes it. All outbound internet traffic was failing on affected units, including the update check — a hardware crypto engine was being starved of one particular kind of memory and the encryption layer could not start at all.
- 52 KB of internal memory turned out to be held by the firmware's own tables, in the one pool that is genuinely scarce. The free-memory low-water mark went from 0 KB to 32 KB. That single cause explains the SD card refusing to remount, USB failing to re-open after a radio power-cycle, and reboots after about an hour of FT8. Thanks Dennis WN4FLA — your three log files were what made it findable.
- Power-cycling the QMX could leave the Tab5 with a frozen screen and no way back except a reboot. It was pinning a CPU core on the dead USB connection. Radio off and on now reconnects in a couple of seconds.

**WiFi remembers your networks** — up to six, automatically, with nothing to type in. If the network it is set to is not there, it finds a remembered one that is, within seconds. Picking a known network from Scan fills in its password. Roy KI0ER asked for this.

**Roy KI0ER's FT8 report — four of the five turned out to be real bugs.** A partner still asking for your final now gets it re-sent up to six times, and once that is used up the Tab5 stays silent rather than calling CQ over the top of him. The occupancy strip no longer fills up permanently during a long CQ run. It now says EVEN / ODD / BOTH so you know which time window you are looking at. And when hunting it gives the other time window a turn.

**Brian WA6JFK** found that the QRZ / eQSL / LoTW setup was hidden until you had logged a contact — backwards, since you want it set up before you operate. It is always visible now.

Two things I would appreciate feedback on:

1. Roy's remaining observation — decodes drying up after an hour or so of continuous operation, cured by a restart — is not yet explained. It may have been the memory shortage above. If you see it on v1.4.0, please say so, because that tells me it was something else.
2. The live spots lane is new. If the callsigns are in the way, or you want them somewhere else on the screen, say so — it is easy to move.

Full technical detail, including what is still unverified on air, is in docs/version-history.md.

73 de OZ1LAV
Steffen
