# v1.8.0 release announcement — draft for groups.io

**For the operator to review and post.** Plain text, no markdown, ready to paste into the
QRPLabs thread "QMX/QMX+ Panadapter for M5Stack Tab5". Links must be checked after the
GitHub release exists.

---

v1.8.0 is out.

RIT you set by tapping. While you are running a frequency and someone answers a couple of
hundred hertz off it, you no longer have to choose between losing them and moving the
frequency everyone else is listening on. Tap RIT at the top right of the spectrum, then tap
the caller: a magenta marker shows where you are now listening, the gold line goes on
marking the dial — which is where you transmit — and the filter window moves onto them. It
stays armed, so the next caller is another tap, and tuning anywhere clears it. Roy KI0ER
asked for this; Michael KZ4LY and Bill Carver shaped how it behaves.

SOTA spots. Summit activations now appear on the spectrum beside POTA, RBN and the DX
cluster, from spothole.app — Ian Renton M0TRT's aggregator, used with his permission. Off
by default, because it is a volunteer-run server and nobody should poll it who has not
asked.

Fox/Hound, hound side. Off, Guided or Automatic. The Tab5 calls from above 1000 Hz, moves
onto the Fox's own frequency the moment it is answered, and stops on the Fox's RR73. It
recognises a Fox by watching it work a queue, not by frequency alone. Simulation-verified
only so far — no real DXpedition has seen it yet, so treat it as new.

The browser caught up with the Tab5. RIT, starting and stopping a POTA/SOTA activation with
a badge while one is running, and the last of the settings that were Tab5-only. The
spectrum and waterfall are now drawn the way the Tab5 draws them rather than approximately,
so black level, contrast, colour scheme and smoothing finally mean the same thing on both
screens. Only the clock-sync window is still Tab5-only.

Two things worth checking if you use the browser.

SWR protection set from the browser was never saved — on any v1.7.x build. The field was
shown, took your value, said it had saved, and the device threw it away. If you set it
there, please check it on the Tab5 under Settings, "SWR protection (transmit)". If it says
Off and you thought otherwise, that was this bug and not you. My apologies: a control whose
whole job is to stop a transmission into a bad antenna is the last one that should fail
quietly.

Spot labels were stealing clicks. Clicking a callsign takes you to that station, which is
intended — but the clickable area was as wide as the callsign and sat at the same height as
the trace, so a long call owned several kHz of spectrum in which every click went to that
station instead of the frequency you clicked. Samuel W7STF found it. Fixed, and hovering a
label now shows where the click will actually take you.

Also fixed: Bluetooth mice that send a different byte layout than the one on my bench (the
cursor pinned itself to the top of the screen — the Tab5 now reads the descriptor the mouse
publishes instead of assuming); two browsers no longer fight over the live spectrum, the
second one is told rather than left flapping; and "Release radio" is reworded to say what
you are trying to do — "Let me use the QMX menus".

The CW transmit offset is narrowed to ±300 Hz and the guidance corrected. Earlier releases
suggested 400–600 Hz, which is outside many operators' filters altogether. Around 100 Hz or
less is what actually works — the aim is to land inside the other station's passband, and
most CW operators run 500 Hz or narrower. Roy KI0ER, who asked for the feature, uses +60.

Download, full change list and the user guide:
  https://github.com/SteffenLav/qmx-panadapter/releases/tag/v1.8.0
  https://tab5.lav.dk

Two things I have not been able to verify myself, so I would rather say so than let you
find out. The RIT display sign is derived from the maths, not checked against a signal — I
have no antenna where I am. If you engage RIT on a steady carrier and the trace jumps
instead of standing still, tell me and it is a one-character fix. And Fox/Hound has only
ever run against simulated stations.

73
de Steffen OZ1LAV
