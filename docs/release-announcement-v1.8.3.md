# Release announcement — v1.8.3 (draft for the operator to post)

Plain text, for groups.io. Nothing below is formatted for markdown.

---

QMX/QMX+ Panadapter for M5Stack Tab5 — v1.8.3

This one is entirely yours. Every fix in it was reported by someone using the radio, and most of them by one person.

You can change your QRZ or eQSL login from the browser. Until now the prompt only ever appeared when nothing was stored, so a key typed wrongly, or one the service later reissued, could not be replaced from the page at all. There is now a "Change QRZ API key" row and a "Change eQSL login" row under the upload links. If you are on an older version and stuck with a wrong key, you do not need to reflash: Files, Config download, edit the qrz_key, eqsl_user or eqsl_pass line, then Files, Config upload. Thanks to Brian WA6JFK.

The dB scale labels now follow the range you set. They were fixed at -40 down to -120 and ignored your Min and Max, so any range other than the default was described by labels that did not belong to it. A narrow range now gets finer lines rather than fewer. At the default range nothing changes.

The Adaptive floor slider is gone. It could not change anything: the noise floor it blends towards is re-seeded many times a second, so both ends of the slider gave the same picture. It was already absent from the browser. A control that cannot do anything is worse than a missing one.

The shaded passband is drawn where the radio actually filters. There was a gap of about 250 Hz between your dial frequency and the start of the shading, because that edge was a fixed number that never came from the radio. In the digital modes the QMX uses one fixed filter of 150 to 3200 Hz, so the shading was being drawn at 200 to 2900.

RF gain no longer sticks on "reading...". It was showing the answer to the previous question, so it only ever filled in when you next opened the drawer, and a single unanswered query left it stuck for the session. It also says "radio not connected" now when the radio is not there, instead of implying it is being read.

The dark bands at the edges of a zoomed view are about half as wide. That is the filter used when zooming, which started rolling off just inside the edge of what is drawn. The filter is now twice as long. I did not simply widen it, because that would have traded a dark edge for false signals appearing where nothing is transmitting.

Out of band, the band strip is now a coarse tuner. Inside a band the strip is a map and you touch where you want to go. Outside one there is nothing to map, so there is a handle in the middle: drag it off centre and the dial moves, let go and it springs back. A full pull moves by half of what is on screen, so two drags overlap instead of skipping a gap, and it gets finer as you zoom in.

The browser's decode list now shows distance and bearing, in the same order as the Tab5, switching to miles if you have that set. Thanks to Tony Abbey.

Most of the rest came from Samuel W7STF, who has now found more real faults than anyone else this month.

Two things I want to be straight about:

The phantom CW signals some of you have seen — a mirror copy of a real signal that only a Tab5 restart clears — are NOT fixed yet. v1.8.2 added a counter so the log would say plainly whether my explanation was the right one. It has not fired once so far, though the only long test ran with no antenna and with the Bluetooth mouse switched off, which is not the condition it was reported under. If you see it, please send a diagnostic log. Until then, restart the Tab5 rather than the radio.

If your QMX stops answering after the Tab5 restarts, power-cycle the radio. That is a known one and the panadapter no longer tries to "fix" it by cutting the radio's power, which is what it used to do.

Download: https://github.com/SteffenLav/qmx-panadapter/releases

Full detail: docs/version-history.md in the repository.

73
Steffen OZ1LAV
