# GitHub issue replies for v1.8.7 — POSTED 2026-08-19

Markdown is fine here (unlike the groups.io posts, which are plain text).

---

## Issue #12 — Mouse issues (KW6E)

Fixed in **v1.8.7**, and your diagnostic logs are what made it possible — thank you.

Two things were wrong:

1. **Your mouse is Bluetooth, and the mouse fix in v1.8.4 went into the USB code.** So
   it never applied to you at all.
2. The movement itself was being read with the wrong layout. Your mouse sends nine-byte
   reports with 16-bit movement; the firmware assumed the packed layout of a different
   mouse, where the two axes share half a byte. Your own report
   `00 06 00 0b 00 ff ff 00 00` is X=+6, Y=+11, and that assumption turned it into
   X=−1280, Y=0 — a big jump the wrong way and no vertical movement. Scrolling was
   unaffected because the wheel byte lands in the same place either way, which is why
   it worked perfectly while the pointer did not.

The decode now picks between layouts that have each been captured off real hardware,
and the same code serves the USB and Bluetooth paths — the USB one had the same
assumption waiting for the next mouse.

⚠ I do not own a Surface Arc, so this is verified against the data in your logs rather
than against your hardware. Please tell me whether the pointer behaves now. If it is
still wrong, a fresh diagnostic log will show me the layout it picked.

---

## Issue #11 — Feature Request: option to manually respond to callers (ericmoritz)

Done in **v1.8.7** — thanks for the kind words and for a well-described request.

It is a checkbox called **Pick callers myself** in the FT8 Filter modal. With it on, a
station answering your CQ does not start the exchange: they wait in the pile-up until
you tap them, and from there the exchange runs itself through to `73` and a log entry,
exactly as before. Only the choice is manual — you are not signing up to send each
message by hand.

Two details you may care about while activating:

- **It keeps calling CQ while you decide**, so the pile-up carries on building. A radio
  that went quiet while you thought about it would look like it had stopped.
- **It overrides "Auto-work pileup"**, since both settings decide who to work next.

Off unless you turn it on.

⚠ Verified in simulation, both with the option on and off, but not yet on a real
activation. If it misbehaves in a live pile-up please reopen this and tell me.
