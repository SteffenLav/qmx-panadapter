# Gestures & Controls

The entire app is controlled via **one-finger swipes from screen edges** and **taps on the top bar**.

## Edge Swipes

| Gesture | From | Effect |
|---|---|---|
| Swipe → | **Left edge** | Toggle Panadapter ↔ FT8 view |
| Swipe ← | **Right edge** | Open settings drawer |
| Swipe ↑ | **Bottom edge** | Open memory channel picker |

Slim **breathing grip handles** on each edge show where to swipe.

## Top Bar Taps

Tap any item on the top bar to open its selector:

| Item | Selector |
|---|---|
| **Frequency** | Numeric keypad (MHz format) |
| **Mode** | USB / LSB / CW / DiGi buttons |
| **Bandwidth** | Filter width (SSB) or CW passband |
| **Band** | Band list (160 m – 10 m + custom) |
| **S-meter** | Peak-hold toggle |
| **Zoom** | 1x / 2x / 4x / 8x presets |

## Spectrum / Waterfall

| Gesture | Effect |
|---|---|
| **Tap** | Tune to that frequency |
| **Drag (1 finger)** | Pan left/right |
| **Pinch (2 fingers)** | Zoom in/out |

The frequency grid snaps based on zoom level (10 kHz at 1x, 1 kHz at 8x).

## Memory Channels

**Swipe ↑** from bottom edge → open memory picker.

| Action | Effect |
|---|---|
| **Tap channel** | Recall frequency, mode, and name |
| **Long-press channel** | Edit name/frequency/mode |
| **Long-press + drag** | Move the channel to a different empty slot (data follows your finger) |
| **Drag onto the wastebin** | Delete the channel — the bottom-right cell (channel 32) is a wastebin; drop a channel on it to remove it |
| **Tap empty slot + Save** | Store current frequency to that slot |
| **Tap occupied slot + Save** | Overwrite that channel |

A new device ships with a few example channels, and the first time you open the picker a one-time (~10 s) tour demonstrates the drag-to-move and drag-to-delete gestures.

## Frequency Keypad

Tap the **frequency label** to open the keypad:

- **Digits**: `0–9`, `.` (decimal point)
- **Delete**: Remove last character
- **Clear**: Clear the entire entry
- **Layout toggle**: Switch between **10-Key** (phone dial) and **Phone** (QWERTY)

Enter frequency as `MHz.kHz.Hz` (e.g., `14.074` for 14.074 MHz).

## FT8 Decode List

| Gesture | Effect |
|---|---|
| **Tap row** | Reply to that CQ |
| **Long-press row** | (reserved for future use) |
| **Scroll** | List is scrollable; swipe up/down |

## Filter Modal

In FT8 view, tap **Filter** to open:

| Control | Effect |
|---|---|
| **Include/Exclude checkboxes** | Toggle filters on/off |
| **Text fields** | Edit filter criteria |
| **Dropdown** | Change auto-reply priority |
| **Save** | Apply changes |
| **Cancel** | Discard changes |

## Settings Drawer

Swipe ← from right edge. The drawer contains:

- **Sections** — tap to expand/collapse
- **Text fields** — tap to edit (opens keyboard if needed)
- **Toggles** — tap to on/off
- **Buttons** — tap to open modals (Config, Time, etc.)
- **Close** — swipe ← again or tap outside

All changes are saved automatically.

## Modals (Pop-up Dialogs)

Standard modal controls:

| Action | Effect |
|---|---|
| **Save / Apply** | Confirm and close |
| **Cancel** | Discard and close |
| **Tap outside** | (varies — some close, some don't) |

Each modal is self-contained — no stacking (you can't open two modals at once).

## On-Screen Keyboard

Appears when you tap a text field:

| Key | Effect |
|---|---|
| **Shift** | Toggle case (abc → ABC) |
| **Backspace** | Delete last character |
| **Space** | Insert space |
| **Numbers** | Tap to switch to number row |
| **Done** / **OK** | Close keyboard and apply |

## Web UI (Browser)

- **Click spectrum** → tune to that frequency
- **Scroll or pinch** → zoom
- **Number fields** → click to edit, press Enter
- **Buttons** → click to toggle settings

All web controls mirror the Tab5 display.

---

**Next:** See [Troubleshooting](troubleshooting.md) for common issues.
