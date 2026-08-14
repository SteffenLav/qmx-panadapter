# Groups replies — 2026-08-14, second round

Plain text, one reply per person. Copy the block under each name. Nothing below
is formatted for markdown.

No dates or release times are promised anywhere in here, because the release is
not scheduled yet. If it is scheduled before you post, add the timing yourself.

---

## ⚠ THIS SUPERSEDES THREE ANSWERS IN `groups-replies-2026-08-14.md`

That file is still unposted, and today's work has overtaken parts of it. Before
posting either file, check these:

1. **Samuel, dark edges.** The earlier draft says "I can make the filter
   sharper". It is now done — the filter is twice as long and the dark band is
   roughly halved. Say so rather than offering it.
2. **Tony, distance column.** The earlier draft says "on the list". It is built.
3. **Samuel and Roy, phantom CW.** The earlier draft says "I have found the
   cause". ⚠ **Since writing it, the counter I added has read ZERO for an entire
   session with the radio streaming.** That is not a refutation — the session ran
   with no antenna and with the Bluetooth mouse switched off, i.e. none of the
   load Roy suspects — but it does mean the cause is NOT confirmed and the
   wording should not say it is. Samuel's block already says "strong hypothesis
   rather than a proven cure", which is fine. **Roy's block states it as fact and
   should be softened before it goes out.**

---

## 1. Brian WA6JFK

On changing your eQSL details: You do not need to reflash, and you certainly do not need the erase option. On the web page, bottom bar, Files, then Config download. That gives you a file called qmx-config.txt with every setting in it. Open it in any text editor, change the eqsl_user and eqsl_pass lines, save it, then Files, Config upload. Your QRZ key is the qrz_key line in the same file. The new details are used by the very next upload, so there is nothing else to do and nothing to restart.

On one caution: that file holds your passwords in plain text, including your WiFi password, so delete it from your computer afterwards or keep it somewhere you are comfortable with. The upside is that the same file is a complete backup of your station settings.

On why you had to ask at all: You found a real gap, and you were right to expect better. The prompt for those details only ever appeared when nothing was stored, so once a key was saved there was no way to change it from the page at all. In the next release there is a "Change QRZ API key" row and a "Change eQSL login" row underneath the upload links, which appear once something is stored. Thank you for reporting it.

---

## 2. Samuel W7STF

On the two displays not agreeing: The Tab5 is in Flat Spectrum mode, which is the default. That scale is not dBm at all — it is decibels above the measured noise floor, which is why it reads 0 to +30 while the browser shows real dBm. Untick Flat Spectrum in the settings drawer and the Tab5 switches to the dBm scale, and then the two do agree.

On the real fault behind that: The dBm gridline labels were fixed at -40 down to -120 and ignored your Min and Max completely, so with -118 and -13 they were describing a scale that was not there. They are now worked out from whatever range you set. At the default range they come out exactly as before, so anyone who has never touched those sliders will see no change.

On why there are so many handles: Because one of them did nothing whatsoever. The Adaptive floor slider could not change anything: the per-bin noise floor it is supposed to blend gets re-seeded about seventeen times a second, so both sides of the blend are always the same number. I have taken it out rather than leave a control that invites you to tune something that is not running. Black level, Contrast and the FFT window all do what they say.

On the 250 Hz gap: Good catch, and it was wrong at both ends. The start of the filtered region was a fixed 200 Hz that never came from the radio at all. Digital modes do not use the selectable SSB filters — the QMX uses one fixed filter of 150 to 3200 Hz, and it reports 3200 as the top edge rather than as a width. So I was drawing 200 to 2900 where the radio actually is 150 to 3200. That is corrected, and I measured it on the screen afterwards at 150 Hz to about 3225 Hz.

On SSB specifically: I could not find the low corner documented anywhere for the selectable SSB filters, so I have deliberately left that one at 200 rather than guess at it. If you know what it really is, tell me and I will use it.

On RF gain stuck on reading: A real bug, and fixed. It asked the radio for the value and then displayed the answer to the previous question, so the first time you opened the drawer there was nothing to display, and nothing repainted it when the answer did arrive. It cleared only when you next opened the drawer, and if that one question went unanswered it stayed like that for the whole session. Since the gain is stored per band it came back every time you changed band. It now also says "radio not connected" when the radio is not there, instead of "reading", which was simply misleading.

On out of band: I have done what you suggested. Out of band the strip becomes a coarse tuner. There is a handle in the middle; drag it off centre and the frequency moves, let go and it springs back to the middle. Pulling it all the way to either edge moves by half of what is currently on screen, so two drags overlap instead of skipping anything, and it gets finer as you zoom in. That was your point about the display staying contiguous as you scroll, and it is the reason the row is now worth its height.

On the arrow buttons: I have no record of promising >, >> and <, <<, so I think we may be talking about different things. What did you have in mind? The handle itself already carries < and > marks.

On the CW decode display: Noted, and I have not chosen between your two layouts yet. I would rather settle that when I actually build the page than decide it in the abstract now. Your point about the decoded text needing time stamps is a good one and I have written it down.

On CW decode going to the microSD card: Not today — nothing CW related is written to the card at the moment. What the card does hold is the diagnostic log, the QSO log, your configuration and your LoTW certificate, so it is worth having one in anyway.

On the pace of requests: Keep them coming. Four of the things in your last two messages were real faults, and two of them I would not have found on my own.
