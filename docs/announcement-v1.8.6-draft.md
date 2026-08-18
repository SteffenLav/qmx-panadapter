# v1.8.6 announcement — draft

---

**QMX Panadapter v1.8.6 is out, and if you are on v1.8.5 please update.**

https://github.com/SteffenLav/qmx-panadapter/releases/tag/v1.8.6

**v1.8.5 shipped with the browser interface completely dead.** One broken text
string stopped the whole page working — it drew its controls and then did nothing:
no spectrum, no waterfall, no buttons, "disconnected" in the corner. If you use the
browser at all, v1.8.5 gave you nothing. Thanks to Randy N4OPI and Michael KZ4LY for
reporting it within hours. The build now refuses to compile a page that does not
parse, so this particular mistake cannot reach you again.

**A crash that looked like a radio fault.** An overnight test aborted inside the USB
driver after about two hours of perfectly healthy operation. The reboot is not the
expensive part — it happens with the radio still plugged in, which is the one
situation that leaves the QMX unable to reconnect, so the radio then stayed dead
until morning. I would have reported that as "the QMX wedged during the night" too.
The QMX was fine. Fixed.

**CW: the displayed frequency and tap-to-tune are corrected** (Roy KI0ER). A signal
on 7.060.000 appeared at 7.060.040, so tapping it tuned you 40 Hz off and the other
station heard you shifted. Two faults added up: a calibration figure that has
defaulted to the wrong value since before the panadapter read your CW offset from
the radio, and the display rounding to whole analysis bins of 47 Hz each. The
arithmetic now reproduces both of Roy's measurements exactly and both go to zero —
but **I have no absolute frequency reference on the bench, so this is not confirmed
on the air. Please measure and tell me.**

**Radio menus** (Michael KZ4LY): you can see what you are typing past message 9 —
the screen scrolls instead of hiding the field behind the keyboard; the
no-second-port help now tells you to power-cycle the radio; and the two-finger
screen blank works properly instead of about one attempt in ten.

**Also documented** (Stan Dye KC7XE): in the band config table, Enable/Disable
entries accept **E** and **D** typed directly, and the arrows change the value
rather than moving between columns — step onto a numeric column to move across.
That is the radio's own behaviour, and it explains something that reads as broken.

**Not a bug:** auto-answer is off after every restart by design — thanks Roy for
answering that one before I got to it.

73 de OZ1LAV
