# Draft reply — asking Samuel and Roy for BT mouse diag logs

Not sent. One post covers both, since the two mice together are the useful pair:
one that misbehaves and one that works.

---

Samuel, Roy — I've found real bugs in how the Tab5 reads a Bluetooth mouse, and
your two mice are the pair that will confirm it. Could you each send me a
diagnostic log with your mouse connected?

First, the part I can already answer, because it saves Roy some time: your two
Microsoft mice will never work, and it is not something I can fix. The Tab5's
Bluetooth comes from its ESP32-C6 co-processor, and that chip does Bluetooth Low
Energy only — it has no Bluetooth Classic radio at all. Most Microsoft mice of
that generation are Classic. A Classic mouse will never even appear to the Tab5,
because it is not speaking a protocol the radio can hear.

The rule for anyone buying a mouse for this: it needs to be Bluetooth 4.0 or
later, i.e. Bluetooth Low Energy. Roy's MX Master is exactly that. A mouse that
never pairs at all is almost certainly Classic. A mouse that pairs but moves the
pointer badly is a different problem — that one is mine, and it is what I have
been fixing.

What I found, in short, and the main one is embarrassing: the Tab5 asks the mouse
to describe its own button and movement layout, and it was only ever reading the
first 22 bytes of that description. The rest was being silently thrown away, so
the description never made sense and the Tab5 fell back to guessing from a layout
I had captured off one particular mouse months ago. Any mouse that does not
happen to match that one guess moves the pointer wrongly. On my own mouse the
real description is 110 bytes, and with all of it the Tab5 now gets the layout
exactly right.

Two more, both fixed: the Tab5 was treating every notification from the mouse as
movement, including its battery level, which is a fine way to make a pointer jump
to the edge of the screen; and it expected the mouse's report number to arrive
with each report, which is how USB works but not Bluetooth.

Before I tell either of you it is fixed, I want to see what your mice actually
declare — I have been wrong before by reasoning from one mouse to all of them,
and that is exactly the mistake that caused this.

To get me the log:

1. Power up the Tab5 and let it connect to your mouse. Move the mouse around for
   ten or fifteen seconds, including a scroll and a couple of clicks.
2. On the Tab5's web page, open the **Files** menu at the bottom and click
   **Diagnostic download**. It saves two files.
3. Send me both, and tell me the make and model of the mouse.

The lines I need are near where the mouse connects — they look like
`report map [67]: 05 01 09 02 ...` and `report layout: id=2 X @bit8/16b ...`.
If the download is awkward, even a photo of those two lines from a serial capture
would do.

Roy, yours matters as much as Samuel's: your MX Master works today, and I need to
be sure the rewrite does not break the one configuration I know is good.

73
Steffen
