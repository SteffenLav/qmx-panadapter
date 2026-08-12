# Draft — to Stan KC7XE: QMX keeps showing both VFOs after split is switched off

Not sent. Written to be reproducible in a few commands, since Stan can verify over
CAT and with a serial monitor.

---

Stan, I have something I think is a QMX display-state issue rather than anything on
my side, and I would value your check before I take it further.

**What I am doing.** The Tab5 panadapter offers a CW transmit offset, so a QRP
station answering a CQ is not zero-beat with everyone else. The QMX has no XIT (the
CAT manual says so plainly — `IF;`'s XIT field is always 0), so I implement it as
split: receive on VFO A, hold VFO B at A + the offset, `SP1;`. That part works.

**The problem.** When the offset is switched off, the radio goes back to simplex but
its LCD keeps showing both VFOs for the rest of the session. My exit sequence is, in
this order:

    FB<VFO A frequency>;    (put VFO B back to match A — must be sent while split
                             is still on, the radio will not take it afterwards)
    SP0;                    (split off)
    FR0;  FT0;              (VFO mode = A)

After that the radio *reports* it is in VFO A mode and simplex:

    SP;  ->  SP0;
    FR;  ->  FR0;
    FT;  ->  FT0;

and the manual says `FT;` returning 0 "must be VFO Mode A". But `LC;` shows the
display has not followed. Reading the LCD at each stage (this is on 1.004.004):

    clean, after power-on        LCA14,024,50                 12:12
    split engaged                LCA14,024,50       14,024,56 12:13
    after my exit sequence       LCA14,024,50       14,024,50 12:13   <- still both
    after MU;                    LCA14,024,50                 12:15   <- single again

**What I have already ruled out.** None of these clear it — the second frequency
stays on the display in every case:

- `FT1;` then `FT0;`, and `FR1;` then `FR0;`. I tried these on the theory that the
  radio ignores a write of the value it already holds (as it does for the SSB filter,
  where re-asserting the current mode digit does not reload it). Not the cause: the
  indicator duly flips `A` -> `B` -> `A`, so the writes take effect, and the dual
  layout persists regardless.
- Re-toggling `SP1;` then `SP0;`.
- Changing operating mode (`MD`) — the exit happens on a CW-to-Digi change anyway.

So the only things that resync the display are `MU;` and a power cycle. The power
cycle is worth noting for what it implies: the dual-VFO layout is not stored in
EEPROM, it is runtime state that survives every CAT route back to VFO A and clears
only on a configuration reload or a restart.

**Which brings me to the second half, and this may be the more useful part for
Hans.** `MU;` also silently drops IQ mode. `Q9` is session state, not written to
EEPROM, so after `MU;` the radio carries on streaming USB audio at full rate — I
measured 47-48k sample pairs a second, unchanged — but it is no longer I/Q, so the
panadapter's spectrum goes flat. `Q9;` reads back `0`, and `Q9 1;` restores it. That
caught me out precisely because the audio *looked* healthy.

For any CAT client that depends on session state, `MU;` is therefore a trap: it is
documented as reloading configuration parameters, and it does, but it takes session
state with it. Even if the display behaviour turns out to be intended, that part
seems worth documenting.

**What I would like to know:**

1. Does your QMX (or QMX+) do the same? Engage split with VFO B different from A,
   then `SP0;` and `FR0;`, and read `LC;`.
2. Is there a command that resyncs the VFO display without discarding session state?
   If there is, I will use it and this is my mistake.
3. If there is not, it looks like the display is not being redrawn when the VFO mode
   goes back to A over CAT — the radio's reported state and its display disagree, and
   only a full config reload reconciles them.

I have not "fixed" this from my side on purpose. I could send `MU;` and then re-run
the IQ handshake, but that means a configuration reload plus a re-handshake every
time an operator switches the offset off, and if the handshake failed they would lose
the spectrum entirely — a poor trade for a cosmetic display. So for now I am leaving
the radio correct and the display wrong, and telling users that both VFOs may remain
on screen but the radio is not transmitting off frequency: VFO B has been set equal
to VFO A and split is off, and I verify both by read-back.

`LC;` deserves a mention, by the way — being able to read the actual LCD contents is
what turned this from guesswork into something I could measure. I had been inferring
the display state from `FR;`/`FT;` and getting it wrong.

If it helps, I can run any sequence you want to see on my bench and send you back the
`LC;` contents and the read-backs — I have a way to fire arbitrary CAT at the radio
and log its replies, so you can direct the measurement rather than repeating it all on
yours. Same for anything you want checked around `MU;` and session state.

73
Steffen OZ1LAV
