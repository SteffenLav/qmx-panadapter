# Draft groups.io announcement — v1.5.0

For the operator to review and post to the QRP Labs groups.io thread
"QMX/QMX+ Panadapter for M5Stack Tab5". Nothing posts this automatically.

---

**Subject:** QMX Panadapter v1.5.0 — the manual answers questions now, instead of being a manual

v1.5.0 is out: https://github.com/SteffenLav/qmx-panadapter/releases/tag/v1.5.0

Download the flasher zip, unzip, run flash.bat (Windows) or flash.command (Mac/Linux). A normal flash keeps your settings, log and certificates.

This release is about the thing a new operator hits first: not knowing what something is called, and so not being able to look it up.

**The manual opens where you are.** The User Manual button in the settings drawer used to drop you on a contents page to search. It now opens the chapter for the screen you were on — the panadapter, the FT8 receive chapter, or the FT8 transmit chapter if you have a transmission armed or running, because someone mid-transmission is asking a different question. Warnings became buttons as well: tap the red "IQ mode not confirmed" banner, or the Need help? button under "Waiting for QMX", and you land on exactly that subject.

**"Need guidance?" — say it in your own words.** A second drawer button opens a short list of symptoms and questions written the way you would actually say them: "My radio is not showing up", "Nothing appears in the decode list", "It never transmits", "How do I change what my CQ says?", "Where are my contacts logged?" Pick one and the manual opens at the answer. You never need to know that the first of those is a CAT link problem, or which chapter the CAT link lives in.

Rows the Tab5 can see are happening right now are highlighted and moved to the top — no radio, IQ mode never confirmed, an empty decode list, WiFi on but not connected. It ranks; you choose. It will not jump you into a chapter because it decided it knew what you meant. And WiFi you have deliberately switched off for POTA is not reported to you as a fault.

**The built-in manual also stopped failing on well-used devices.** A few people saw "could not cache the page". The manual was being copied out of the firmware into the Tab5's internal storage just so it could be read back — and on a device that has been used, that storage is already full of your QSO log, your LoTW certificate and key, and the diagnostic log. The copy is gone; pages are read straight out of the firmware. Nothing left to fail. Missing characters are fixed too — the waterfall and occupancy sketches in the manual were not drawing boxes, they were vanishing and leaving blank space with a caption under it.

**Call CQ from the browser.** Dennis WN4FLA asked for this: a CQ run that has timed out, or that has reached its call limit, otherwise needs a walk back to the Tab5. There is now a Call CQ button under the web page's TX status banner. It asks for confirmation first, because it keys the radio, and it uses exactly what the Tab5 would — your active CQ preset, your current TX tone including TX Hold, and your EVEN/ODD choice.

**The station you are working stays at the top of the decode list.** Don WB0LQW: "there is no station that I am as interested in as the one I am trying to contact." During an exchange their messages used to sort down-screen where you had to hunt for them, including the case you most want to see — them answering somebody else. It releases as soon as the QSO completes.

Two things I would appreciate feedback on:

1. Don's decode-list change and the web Call CQ button are both unverified on the air — I have no antenna up at the moment. Both are confirmed working on the bench as far as the bench can go, and the decode-list pin writes a line to the diagnostic log when it engages, so if you make a contact on this version that log will settle it.
2. The wording of the guidance-panel rows. They are written the way I imagine an operator describes a fault, which is not the same as the way you actually describe it. If a row makes no sense to you, or the one you needed is missing, tell me — that is a documentation bug and I will fix it.

Also in this release, for BD4AHS: the auto-answer picker's "skip stations I have already worked" is a setting, not automatic — it follows the Exclude worked before tick box in the same Filter window. My own notes said otherwise, which is probably why it surprised you. They are corrected.

Full technical detail, including everything still unverified on air, is in docs/version-history.md.

73 de OZ1LAV
Steffen
