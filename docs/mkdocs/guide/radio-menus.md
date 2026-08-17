# Radio Menus

The QMX's own menu system, on the Tab5's screen — the same 80×24 terminal you
would get from a laptop running PuTTY, without the laptop.

This exists because a QMX+ with no control panel has **no other way in**. If you
built a headless radio to save weight and space, this is how you reach
Configuration, Band config, System config and everything else.

It is equally useful on a radio that *does* have a panel: two lines of LCD and a
rotary encoder is a slow way to move around a deep menu, and the Tab5 shows the
whole thing at once.

---

### 1. First: give the radio a second serial port

The terminal runs on the QMX's **second** USB serial port, and that port is off by
default. You only have to do this once — the setting survives a power cycle.

On the radio: **System config → GPS & Ser. ports → USB serial ports → 2**

!!! note "Why a second port, and not the one already in use"
    The QMX manual is explicit that leaving a terminal session without choosing
    *Exit terminal* leaves the radio refusing CAT commands. On the port the
    panadapter is already using, that would take the whole display down with it.

    On its own port it cannot. CAT frequency, mode and the S-meter were measured
    running normally for the entire length of a terminal session — the panadapter
    keeps working while you are in the menus.

If the port has not been enabled, the Tab5 tells you so rather than failing
quietly — and it puts **the whole menu path on screen**, in both the Tab5 panel and
the browser, laid out to be followed while you are looking at the radio. The person
who needs that instruction is the one who never read this page, so it belongs where
the failure happens rather than in a message that disappears. *(Michael KZ4LY)*

---

### 2. Opening it

=== "On the Tab5"

    **Settings drawer → Radio → Radio menus**

    The keys are along the top: **▲ ▼ ◀ ▶**, **Enter**, **Back**, and **Close**.

=== "In the browser"

    **Radio ▸ Radio menus**

    The on-screen buttons work, and so does your actual keyboard — arrow keys,
    Enter, Esc, and typing. For entering a value into a menu field, the keyboard
    is much the faster route.

The radio's menus **cascade**: opening a submenu draws a new box over the top of
the one you came from, so you can see the path you took. The highlighted row is
where you are.

---

### 3. Leaving it

Use **Close**.

The Tab5 walks the radio back out through its own *Exit terminal* item — it reads
the screen to find it, so it still works however deep in the menus you were. That
matters: simply dropping the connection is what leaves a QMX ignoring CAT.

Two things happen automatically so you cannot get stuck:

- If you close the browser tab, the session is closed for you.
- If nothing happens for **two minutes**, the Tab5 hands the radio back by itself.
  You will see *"session timed out — closed"*. Open it again to carry on.

!!! tip "Where you land"
    A session opens on the **first** menu item, not on *Exit terminal* — even
    though *Exit terminal* is where the previous session left the highlight. That
    is deliberate: otherwise your first Enter would drop you straight back out.

---

### 4. Typing and editing a value

A **block cursor** shows where you are typing — in a field like one of the Messages,
that is the difference between editing and guessing. *(Randy N4OPI)*

For deleting, there are **two separate keys, BS and DEL**. They send different bytes
(0x08 and 0x7F), because a terminal application may want either and the QMX manual
does not say which. In the browser your real Backspace and Delete keys are wired to
one each. If you find which one your firmware actually deletes with, please say so —
then it can become a single obvious key.

!!! warning "Some fields will not change yet, and this is not fixed"
    Values that are more than two digits, or that sit in a table, do not increment
    with ◀ ▶ — in a table the arrows move between columns instead. Reported by
    Randy N4OPI on **Max PA Voltage**, the **band config** table columns, **CAT
    Config → Timeout**, **System config → TCXO frequency**, the double-click
    timeout, and the same fields inside **Virtual U3S**.

    These need some key other than left/right, and rather than ship a guess the
    question has gone to QRP Labs. Everything else in the menus edits normally. Use
    the radio's own controls, or a laptop terminal, for those fields for now.

### 5. What it does not do

- **It does not transmit.** Nothing here keys the radio.
- **It does not know what you changed.** If you alter something the panadapter
  displays — a filter, a band setting — the Tab5 finds out the same way it always
  does, from its next CAT poll.
- **It is the radio's interface, not ours.** What the menus contain, and what each
  item does, is documented in the QRP Labs QMX operating manual.
