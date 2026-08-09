# Draft groups.io announcement — v1.6.0

For the operator to review and post to the QRPLabs thread. Nothing posts
automatically.

---

**QMX Panadapter v1.6.0 is out.**

https://github.com/SteffenLav/qmx-panadapter/releases

The headline is that the web page stopped being a viewer. It could always show
you the band; now it can work it. Tap a station and it replies with the right
message, the same decision the Tab5 makes, with a confirmation first because a
mis-click from another room should not key your radio. Live spots on the
spectrum, the TX tone picker, memory channels, settings you can type on a real
keyboard, the pileup and grey-list views, Antenna Tune, and the whole manual —
all served by the Tab5 itself, no internet needed.

It also answers to **qmx.local** now, so you no longer have to know its IP.

For the CW operators, and thanks to Roy KI0ER for the request: a **CW transmit
offset**. Everyone answering a CQ zero-beat arrives as one mud-pit, and a QRP
station 400–600 Hz off stands out. Set it once and it follows you — a tap on the
panadapter, a spot, a memory recall, a band change, the web page, and the
radio's own tuning knob. The QMX has no XIT, so it is done with split, and it
applies in CW only.

From Stan KC7XE via Samuel W7STF: **RF gain** on a slider, read back from the
radio because it is a per-band setting; and **Release radio**, which stops the
Tab5 talking to the QMX so you can use the radio's own menus and its Band
Configuration terminal without the two fighting over the same port.

That last one led somewhere useful. If you have ever had the decode list go
blank while transmit still worked, the cause is now understood: a trip through
the QMX's own menu can restart the radio, which switches its IQ mode off — so
you are left with a radio that answers every command and sends no audio. The
Tab5 now just asks for IQ mode again after 30 seconds of silence, and only
escalates if that does not help.

Roy also found three things in the FT8 transmit window, all fixed: the occupancy
strips show both time windows, the "FREQ BUSY" warning stopped counting stations
in the window that cannot collide with you, and the auto-answer robot moves on
from a station that turns out to be working somebody else.

The built-in manual gained an **A–Z index**, and its diagrams are properly drawn
now instead of being made of box characters — on the Tab5 and on the website.
Redrawing them turned up seven statements that were no longer true, including an
FPS readout the Tab5 has never had.

**Three things are not yet confirmed on the air**, and I would rather say so than
let you find out: the CW transmit offset (the radio does the right thing on the
bench, but nobody has made a contact with it yet), the CQ listening slot's
cadence, and the audio-recovery watchdog against a real occurrence in the field.
Reports on any of the three are welcome.

Install: download the flasher zip from the releases page, unzip, run flash.bat
(Windows) or flash.command (Mac/Linux). Settings, QSO log and certificates are
preserved.

Full detail: https://github.com/SteffenLav/qmx-panadapter/blob/main/docs/version-history.md
Guide: https://tab5.lav.dk

73 de OZ1LAV
