# Draft — reply to Roy KI0ER on his VFO B workaround

Not sent. Short on purpose. Stan reads this thread, so the useful part is the clue,
stated once.

---

Roy, that helps more than you might think. Thank you.

A band change makes the QMX load that band's configuration, so it is a config reload.
That is the only other thing I found which redraws the VFO display — the CAT command
`MU;` does it too. Everything else I tried left both VFOs on screen.

The interesting part is that your band switch clears the display without breaking
anything, while `MU;` also switches off the radio's IQ mode and flattens the
panadapter. So there look to be two reload paths in the radio, one that keeps the
session state and one that does not. That is a much smaller thing to ask about than
"please fix the display".

I will pass it on. Your workaround is a good one in the meantime.

73 Steffen
