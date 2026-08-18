Reply to Markus DL8MBY - VFO B issue. Plain text, ready to paste.

---

Markus - nothing to apologise for, that is a real bug and you diagnosed it correctly.

The panadapter reads and writes VFO A only (FA over CAT). With your QMX+ receiving on
VFO B, every frequency the Tab5 set went to a VFO you were not listening to - while
band select still worked, which is exactly why it looks like a configuration mistake
rather than a fault. Your point about the small VFO indicator is the reason it needs
handling in software, and it will be: the next release checks the radio's VFO mode
when it connects, switches it to A if needed, and tells you on screen that it did.

Until then, on the radio:

To get back to VFO A now - a single short press of the Exit button cycles the VFO mode
(A, B, Split). Press it until the display shows A.

To stop it happening again - go to Configuration, then VFO, then VFO modes. There are
three entries: VFO A, VFO B and Split. Set B and Split to DISABLED and leave A
ENABLED. The Exit button then has nothing else to cycle to, so you cannot land on the
wrong VFO by accident.

Two things worth knowing: the QMX re-enables all three if you ever disable every one
of them, so it cannot get stuck; and if you want split later, just re-enable it there
- the panadapter is happy with split, because split receives on VFO A.

73 and thanks for the kind words,
Steffen OZ1LAV
