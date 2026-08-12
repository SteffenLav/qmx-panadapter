# Draft — to Stan KC7XE: QMX keeps showing both VFOs after split is switched off

Not sent. Kept short but reproducible.

---

Stan, I think I have a QMX display-state issue and I would value your check before I
take it further. This is on 1.004.004.

The Tab5 panadapter offers a CW transmit offset so a QRP station is not zero-beat with
everyone else. The QMX has no XIT, so I do it with split: receive on VFO A, hold VFO B
at A plus the offset, `SP1;`. That works.

Switching the offset off is the problem. My exit sequence is:

    FB<VFO A frequency>;    (VFO B back to match A - must be sent while split is
                             still on, the radio will not take it afterwards)
    SP0;                    (split off)
    FR0;  FT0;              (VFO mode A)

The radio then reports `SP;`=0, `FR;`=0, `FT;`=0, and the manual says `FT;`=0 "must be
VFO Mode A". But `LC;` shows the display has not followed:

    clean, after power-on        LCA14,024,50                 12:12
    split engaged                LCA14,024,50       14,024,56 12:13
    after my exit sequence       LCA14,024,50       14,024,50 12:13   <- still both
    after MU;                    LCA14,024,50                 12:15   <- single again

Already ruled out, all leaving the second frequency on screen: `FR1;`/`FR0;` and
`FT1;`/`FT0;` (the indicator flips A to B to A, so the writes are not being ignored),
re-toggling `SP1;`/`SP0;`, and changing operating mode. Only `MU;` and a power cycle
clear it, so the dual layout looks like runtime state rather than anything in EEPROM.

**`MU;` also silently drops IQ mode**, and that may matter more than the display. `Q9`
is session state, so afterwards the radio still streams USB audio at full rate while it
is no longer I/Q. My spectrum went flat. `Q9;` reads 0 and `Q9 1;` restores it. For any
CAT client that relies on session state, `MU;` is a trap worth documenting.

Three questions:

1. Does your QMX or QMX+ do the same? Engage split with B different from A, then `SP0;`
   and `FR0;`, and read `LC;`.
2. Is there a command that resyncs the VFO display without discarding session state? If
   there is, this is my mistake.
3. If not, it looks like the display is not redrawn when the VFO mode returns to A over
   CAT.

I have not worked around it. `MU;` plus a fresh IQ handshake on every stand-down would
risk the spectrum to fix a cosmetic display. So the radio is left correct and the
display wrong, and I tell users the radio is not transmitting off frequency: VFO B
equals VFO A and split is off, both verified by read-back.

73
Steffen OZ1LAV
