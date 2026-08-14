# Groups replies — 2026-08-14

Plain text, one reply per person, in order of appearance. Copy the block under
each name. Nothing below is formatted for markdown.

Release goes out tonight.

---

## 1. Samuel W7STF

On the phantom CW signals: I have found the cause, and it is on the Tab5 side, not in the radio. The audio arrives as pairs of I and Q samples. If a read ever ends halfway through a pair, the leftover bytes were thrown away, and everything after that point was off by a few bytes for the rest of the session. That shifts I against Q, which is exactly what produces a mirror copy of a real signal. It cannot repair itself, and it survives a radio restart, because the fault is in my buffers and not in the QMX. That matches what you and Roy both saw.

On the fix: The leftover bytes are now kept and joined to the next read. I have also added a counter so the log states plainly whether this is really happening on your unit. I am calling it a strong hypothesis rather than a proven cure until a log from one of you confirms it. New firmware tonight.

On the workaround until then: Reboot the Tab5, not the radio. Roy found that independently and he is right.

On the dark edges at the ends of the spectrum and waterfall: Measured, and your estimate was accurate. It is the filter used when zoomed in. It starts rolling off just inside the edge of what is drawn, so the outer part of the view is attenuated. At the zoom in your screenshots the extreme edge is about 10 dB down and the affected part is roughly 1200 Hz at each side. You said about 1000 Hz, so you were reading it correctly.

On whether that is new: It is not. It has behaved this way since zoom was added. It is a property of the filter, not a fault. I can make the filter sharper, which narrows the dark band. Widening it the easy way would trade the dark edge for false signals, so I will not do that.

---

## 2. Tony Abbey

On the distance column in the browser FT8 list: Agreed, and the Tab5 already works the distance out. It is only a matter of sending it to the browser and adding the column. On the list.

---

## 3. Roy KI0ER

On your reboot test: That was the decisive piece of information, so thank you. Restarting the QMX changing nothing while restarting the Tab5 fixed it told me the fault holds state on my side. It ruled out the radio and it ruled out my first theory.

On the cause: The audio arrives as pairs of I and Q samples. If a read ended halfway through a pair, the leftover bytes were discarded and everything after that was shifted by a few bytes for the rest of the session. That puts I and Q out of step, which produces a mirror copy of a real signal, keying gaps and all. Nothing re-synchronises it, and the USB audio session stays open across a QMX power cycle, so only a Tab5 restart clears it. That is your observation exactly.

On the Bluetooth mouse: Your instinct is probably right. The trigger is load, and the mouse adds load, so it makes a partial read more likely. Not proof yet, but a sensible thing to have noticed.

On the fix: The leftover bytes are now carried into the next read, and I have added a counter so a log will say plainly whether this was happening. New firmware tonight. Until then, reboot the Tab5 rather than the radio.

On having something to do: Message received.

---

## 4. Don WB0LQW

This one answers his email as well, so no separate email reply is needed. It
covers: the cause, his test procedure, the two log files, the 3.2 W against
3.62 W reading, his QMX firmware question, and the fact that v1.8.2 will not
fix it.

On your CQ presets: Solved, and your log is what solved it. You were not wasting my time. Everything below answers your email too, so there is nothing further you need from me by mail.

On the cause: Your message 2 is stored with two spaces between CQ and POTA. Message 1 has one space, which is why that one always worked. In FT8 the space is part of the message format, not just spacing. With the extra space the encoder stops reading it as a CQ and encodes a signal report to an abbreviated callsign instead. The radio then transmits a valid frame for the full 12.6 seconds, which is why you saw 3.2 W and a good SWR, and a receiver decodes it as CQ followed by three dots and a report. Nothing WSJT-X can use. With a grid added it will not encode at all, which is why those presets refused to key and sent you to the identity window.

On reproducing it: I recreated all three of your symptoms on the bench from that single character.

On your QMX firmware: Not involved. 1_03_002 is fine. Roy could not reproduce it because his preset does not have the extra space.

On installing v1.8.2: Please do, but it will not fix this one, so expect the same result. That release only changed the error message so it stops blaming your callsign. The actual fix is tonight.

On what you can do right now: Open the CQ editor and delete the extra space in messages 2 and 3. They will work immediately, on whichever version you are running, with no update needed.

On the fixes tonight: Extra spaces anywhere in a message are collapsed, so a preset repairs itself the next time you open the editor and you will see the corrected text. Separately, every message is now decoded back the way the receiving station will see it before the radio is keyed, and if it no longer says what you typed it is refused with the reason instead of transmitted. That second one covers causes I have not thought of yet.

On the two files: That is expected. qmx-log.txt is the live log since the last download. qmx-log-saved.txt is the copy held in flash that survives a power off. Sending both is the right thing to do.

On the power reading: 3.2 W reported by the radio against 3.62 W on your meter is normal.
