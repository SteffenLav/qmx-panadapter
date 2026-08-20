# v1.8.9 — 2026-08-20

**The Tab5 can update itself now, and a transmitter that could be left keyed no
longer can.**

## The radio can no longer be left transmitting

Roy KI0ER reported his QMX transmitting continuously until he power-cycled it,
with the Tab5 running normally throughout. His log had the whole story: a USB
transfer timeout mid-burst, then every command failing — including the two that
*stop* transmission. The burst finished, the radio stayed keyed, and nothing
tried again.

The link recovered about two seconds later. Nothing re-sent the stop command.

That is fixed twice over: the stop command is now retried immediately, and if it
still cannot get through it is handed to the radio-control task, which keeps
re-sending it on every cycle that works until the radio is demonstrably back in
receive. Two seconds or two minutes, it no longer matters.

If it ever happens again the log says so plainly instead of looking like a
healthy transmission.

## Updating from the device

The version at the bottom of the screen now tells you when a newer release
exists, and installs it if you ask. Touch it, hold for a second, let go. It
downloads in the background and does **not** restart on its own — you choose
when. The web page says the same thing in the same words.

Nothing is downloaded without you asking. Your settings, QSO log and LoTW
certificate are untouched, and the previous firmware is kept as a fallback.

Install this release with the flasher as usual; after that the cable is only for
emergencies.

## Also

- **A crash now survives the reboot.** If the Tab5 ever restarts unexpectedly,
  the next boot records what happened — which part of the firmware, how far into
  the session, and where. Send the diagnostic download and that is enough.
- **The diagnostic log is written to a microSD card again while WiFi is on.**
  Previously the card only received a backup at start-up, so the log was not
  there when it was needed.
- **The flasher download is 2.9 MB instead of 44 MB** *(Gyula HA3HZ)* — it had
  been quietly carrying every previous version inside it.
- **FT8 can move off an occupied frequency mid-QSO**, to the nearest clear slot
  only *(Roy KI0ER, Gyula HA3HZ)*. Far enough to escape whoever is on top of
  you, near enough that a station with a narrow receive filter still hears you.
- **A bandwidth reading stuck on a CW filter after switching to LSB** is fixed
  *(Samuel W7STF)*.
- **The out-of-band tuner now works in the browser too** *(Samuel W7STF)*.
- **Browser display stalls**: the message telling a second browser it had lost
  the live view was malformed, so browsers hung up and grabbed it straight back
  *(Samuel W7STF)*.
- **The frequency readout could stick** on an old value while the spectrum and
  the radio were correct.
- **Spur suppression has been withdrawn** from the settings drawer. It only ever
  worked at ×1 zoom, which is not where anyone looks at spurs, and with a real
  antenna the problem is far smaller than bench measurements suggested. The work
  is kept for a future release.
- **Every web API command is documented**, and the error behaviour is now
  described correctly.
