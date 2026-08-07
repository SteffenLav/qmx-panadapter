# Draft reply — orderly-shutdown experiment result (Roger AD5DZ, Stan)

For the operator to review and post to the groups.io thread. Nothing posts this
automatically.

---

Roger, Stan - fair challenge, so I built it and tested it this morning.

The host now has an orderly shutdown: it returns the radio to RX (TA0;RX;),
closes CAT, sets the audio streaming interface back to alternate setting 0 - the
formal "stop producing isochronous packets" signal - closes it, and then drops
VBUS so the device sees a real disconnect. Every step is logged and every step
returned OK. The radio was streaming normally right up to the button press:

    audio: RX 51623 pairs/s
    usb_shutdown: orderly USB shutdown starting (device present: yes)
    cat: shutdown: returning the radio to RX
    cat: shutdown: CAT closed cleanly
    audio: shutdown: UAC stream stopped (ESP_OK) and closed (ESP_OK)
    usb_shutdown: root port power off: ESP_OK
    usb_shutdown: orderly USB shutdown complete - safe to reflash or power off

Then I reflashed the host, and the QMX answered the very next enumeration with
the same empty data stage as before:

    ENUM: [0:0] Unexpected (8) device response length (expected 16)
    ENUM: [0:0] CHECK_SHORT_DEV_DESC FAILED

So the result is negative: a fully orderly teardown - no transfer in flight, no
interface open, VBUS visibly dropped, quiet bus before the host reset - does not
prevent the wedge. There was nothing mid-transfer left to abandon this time, and
it wedged anyway.

One caveat in fairness: the failure is intermittent, so a single trial cannot say
whether the orderly shutdown reduces the odds. But it is clearly not a cure, and
together with the earlier observation that the state survives an 8-second VBUS
cut, it points at state inside the QMX's USB stack that persists across a
complete, observable disconnect - not at anything the host does on its way out.

I will keep the orderly shutdown in the firmware regardless - it is plainly the
right behaviour, and it guarantees the radio is never left keyed by a reboot.
Serial captures of both sides available as always.

73 Steffen OZ1LAV
