# Draft reply: Stan KC7XE and Roger AD5DZ on the USB reconnect wedge

Plain, and it concedes the parts that were mine.

---

Stan, Roger - thanks, both useful.

First, two of the three faults in this were mine, and both are fixed. The Tab5 was
leaving a dead USB device occupying the root port, and its audio task was spinning on
that dead handle hard enough to starve the very task that delivers the disconnect
event - so the radio could not be re-enumerated even when it was perfectly willing.
Suspecting the host side was reasonable.

What is left is narrower, and I described it badly the first time. Corrected:

The line my host logs is "Unexpected (8) device response length (expected 16)" at
enumeration stage CHECK_SHORT_DEV_DESC. ESP-IDF counts its own 8-byte setup packet in
both of those figures, so 8 actual means the setup stage plus **zero data bytes**. It
is not a short device descriptor - it is an **empty data stage** in reply to
GET_DESCRIPTOR(DEVICE) with wLength=8, at address 0, on EP0. My earlier "8 of 16
bytes" was misleading and I should not have phrased it that way.

Roger, on the polling point: that log line is my host reporting what the device
answered to a request it had just sent, so enumeration is definitely running. A
physical unplug and replug - which is exactly the cold attach your Chromebook test
performs - produces a fresh attach and the same empty answer, several times over. And
the Tab5 enumerates the same QMX normally on an ordinary cold start; that is the
everyday case.

I think your test and mine are not the same event. Yours is an orderly reboot, where
the OS shuts the bus down on its way out. Mine is the host USB controller vanishing
mid-transfer, with UAC isochronous audio and CDC-ACM both streaming, because I am
reflashing the ESP32-P4. Nothing on the bus, no suspend, no reset - the host simply
stops existing.

That suggests a test that would settle it. With ARDOP streaming audio and CAT, cut the
Chromebook's power abruptly rather than rebooting it cleanly, then cold start. If the
QMX comes back, the variable is how the host leaves. If it wedges, it reproduces well
away from my hardware and off the M5Stack entirely.

Two more details in case they help: it survives VBUS being cut for up to 8 seconds,
and it is intermittent - a few host restarts reconnect fine - which is what makes me
suspect leftover EP0 state that depends on what the QMX's USB stack was doing at the
moment the host disappeared. Reproduced identically on 1_03_002 and 1_04_004.

Full serial captures available for any of this.

73, Steffen OZ1LAV
