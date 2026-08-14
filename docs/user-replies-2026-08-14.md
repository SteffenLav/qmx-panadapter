# Replies to send — 2026-08-14

From the overnight group traffic. Nothing pushed or posted.

**Status key:** ✅ fixed and verified · 🔧 fixed, needs a field check ·
❓ needs information · 💭 measured, no action yet

---

## 1. Don WB0LQW — CQ presets keying with nothing decodable ✅

> Don, your log found it. You were not wasting my time — you sent the one thing
> that could solve this, and it did.
>
> **The cause is a second space.** Your preset 2 is stored as `CQ  POTA WB0LQW`
> with two spaces between `CQ` and `POTA`. Preset 1 is `CQ WB0LQW DN70` with one
> space, which is why that one always worked.
>
> **Why one space matters that much:** In FT8 the space is part of the message
> format, not just spacing. With the extra space the encoder stops reading your
> message as a CQ and encodes it as a signal report to an abbreviated callsign.
> The radio then transmits a perfectly valid FT8 frame for the full 12.6 seconds —
> which is why you saw 3.2 W and a good SWR — and a receiver decodes it as
> `CQ  <...> +00`. Nothing WSJT-X can do anything with. With a grid added it will
> not encode at all, which is why those two presets refused to key.
>
> I reproduced all three of your symptoms from that one character on the bench.
>
> **Your QMX firmware is not involved,** so `1_03_002` is fine. Roy could not
> reproduce it because his preset does not have the extra space.
>
> **Two fixes:** Extra spaces anywhere in a message are now collapsed, so the
> preset repairs itself the next time you open the CQ editor and you will see the
> corrected text. Separately, every message is now decoded back the way the
> receiving station will see it before the radio is keyed, and if it no longer
> says what you typed it is refused with the reason instead of transmitted. That
> second one covers causes I have not thought of yet.
>
> **Meanwhile, on the version you have:** open the CQ editor and delete the extra
> space in messages 2 and 3. They will work immediately.
>
> Also worth saying: your radio reporting 3.2 W against 3.62 W on your external
> meter is normal, and the two files are expected. `qmx-log.txt` is the live log
> since the last download; `qmx-log-saved.txt` is the copy in flash that survives
> a power-off.

---

## 2. Roy KI0ER and Samuel W7STF — phantom CW signals ❓

> Roy, Samuel — the description fits a mirror image: an exact duplicate of a real
> signal, keying gaps and all, nothing there when you tune to it.
>
> **The detail that points somewhere useful is that a reboot clears it.** The
> panadapter corrects the I/Q balance from the received signal itself, and that
> correction is rebuilt from scratch at every start. If a phantom is absent right
> after a reboot and returns later, the correction is settling on the wrong answer
> and creating the image rather than removing it. On my own bench it settles at
> around 7 dB and 26 degrees, which is a very large correction.
>
> **One test, and it takes a minute.** When you next see a phantom, switch
> **I/Q balance correction off** in the settings. If the phantom disappears with
> it off and comes back with it on, that is the cause and I know where to fix it.
> Nothing to install and it is reversible immediately.
>
> Samuel, on your other question: reboot the Tab5 and the radio separately. My
> expectation is that only the **Tab5** reboot clears it. If the radio reboot
> clears it instead, I am wrong and that is just as useful to know.

---

## 3. Samuel W7STF — dark edges on the spectrum and waterfall 💭

> Samuel, measured, and your estimate was accurate.
>
> **It is the zoom filter, not the display.** When zoomed in, the audio is
> filtered before the spectrum is computed. That filter starts rolling off just
> inside the edge of what is drawn, so the outer part of the view is attenuated.
>
> At the zoom in your screenshots the very edge is about 10 dB down, and the
> affected part is roughly 1200 Hz at each side. You said about 1000 Hz, so you
> were reading it correctly.
>
> **It has always done this** — the behaviour dates from when zoom was added and
> has not changed. It is a fixed property of the filter, not a fault.
>
> **What I can do:** make the filter sharper, which narrows the dark band without
> introducing anything false. Widening it the easy way would trade the dark edge
> for false signals appearing near it, which is worse, so I will not do that.
> Queued, not urgent, unless you would rather have it sooner.

---

## 4. Tony Abbey — distance column in the browser FT8 list 💭

> Tony, reasonable, and the Tab5 already works out the distance, so this is only
> a matter of sending it to the browser and adding the column. On the list.

---

## Notes to self, not for sending

- Don's fault is closed with a harness that links the real `ft8_lib` and
  reproduces all three symptoms. The round-trip guard is the durable half.
- The phantom-CW hypothesis is **unproven**. The I/Q balance test is the
  discriminator. Do not present it to anyone as the cause until an operator
  reports back.
- Zoom filter numbers, computed from the shipped 31-tap design: edge
  attenuation −16.6 dB at ×2, −10.3 dB at ×4, −8.0 dB at ×8, with the rolled-off
  width 1846 / 1237 / 928 Hz per side.
