# Reply to Markus DL8MBY — VFO B issue

---

Markus — nothing to apologise for, that is a real bug and you diagnosed it
correctly.

The panadapter reads and writes **VFO A only** (`FA` over CAT). With your QMX+
receiving on VFO B, every frequency the Tab5 set went to a VFO you were not
listening to — while band select still worked, which is exactly why it looks like a
configuration mistake rather than a fault. Your point about the small VFO indicator
is the reason it needs to be handled in software, and it will be: the next release
checks the radio's VFO mode when it connects, switches it to A if needed, and tells
you on screen that it did.

**Until then, on the radio:**

*Get back to VFO A now* — a single short press of the **Exit** button cycles the VFO
mode (A → B → Split). Press it until the display shows A.

*Stop it happening again* — in the radio's menu:

    Configuration -> VFO -> VFO modes

There are three entries, VFO A / VFO B / Split. Set **B** and **Split** to
**DISABLED** and leave **A** ENABLED. The Exit button then has nothing to cycle to
and you cannot land on the wrong VFO by accident.

Two things worth knowing: the QMX will re-enable all three if you ever disable every
one of them, so it cannot get stuck; and if you *do* want split later, re-enable it
there — the panadapter is happy with split, because split receives on A.

73 and thanks for the kind words,
Steffen OZ1LAV
