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
| **Tap a spot callsign** | Tune to that station **and** set the mode — see [Live Spots](../guide/spots.md) |
| **Tap an off-screen count** (`<3` / `5>`) | Jump to the nearest spot outside the window, on this band |

The frequency grid snaps based on zoom level (10 kHz at 1x, 1 kHz at 8x).

## Band-Plan Strip (Panadapter)

The thin coloured strip just above the bottom status bar shows where you are within the band. The framed **visible-span block** on it is a slider handle you can drag to retune.

| Gesture | Effect |
|---|---|
| **Tap** the strip | Jump to that frequency in the band |
| **Drag** the strip sideways | Scrub along the band — frequency updates live, QMX retunes on release |
| **Drag from the bottom bar** (sideways) | Same band scrub, grabbed on/under the slider handle *anywhere along the bottom status bar* — a taller, easier target |
| **Swipe ↑** from the bottom bar | Still opens the memory picker (vertical = memory, sideways = band-plan — the two share the row) |

## Display Sleep

| Gesture | Effect |
|---|---|
| **Tap** (screen asleep) | Wake the display — the wake tap is swallowed, so it can't tune or press anything |
| **Two-finger double-tap** | Blank the display immediately |

The idle timeout is set via the **Display sleep** dropdown in the settings drawer (see [Settings](../guide/settings.md)).

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

## ADIF Log Viewer

| Gesture | Effect |
|---|---|
| **Long-press row** | Highlight the QSO red and lock list scrolling |
| **Drag up/down** (while highlighted) | Move the highlight to another row |
| **Release** | A Delete/Cancel bar appears — **Delete** removes that one record |
| **Delete all** (tap twice) | First tap arms it ("ALL *N*?"), second tap within 5 s erases the whole log — no undo |

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

Swipe ← from right edge. The drawer is a single scrolling list of sections (they don't collapse — just scroll). It contains:

- **User Manual** (top button) — opens this documentation on the Tab5 itself, at the chapter covering the screen you were on (see below)
- **Need guidance?** (directly below it) — a list of symptoms and questions in plain words; picking one opens the manual at the answer. See [Getting Help](../getting-help.md)
- **Text fields** — tap to edit (opens keyboard if needed)
- **Toggles** — tap to on/off
- **Buttons** — tap to open modals (Config, Time, etc.)
- **Close** — swipe ← again or tap outside

All changes are saved automatically.

## User Manual (on-device docs)

The **User Manual** button at the top of the settings drawer opens this whole guide right on the Tab5 — no phone or laptop needed.

**It is built into the firmware** (since v1.3.2), so it works immediately and always: no WiFi, no microSD card, no download, no waiting, on the very first boot. It also can never describe a different version than the one you are running, because it ships inside it.

- **It opens in context** — the panadapter chapter, the FT8 *receive* chapter, or the FT8 *transmit* chapter when a transmission is armed or running. Not a contents page you have to search.
- **Contents** — a two-column list of every page. Press and slide your finger down it: a highlight bar tracks your finger, and lifting opens the highlighted page.
- **Back** — returns to the previous page you viewed (shown only when there is somewhere to go back to). **Exit** — leaves the manual (back to the panadapter/FT8 screen).
- **Edge swipes and top-bar taps are stood down** while the manual is open, so a stray touch cannot retune or switch view behind it.
- **Hold the drawer's User Manual button for 3 s** to reset the reader. The manual itself is untouched — it is part of the firmware and cannot be lost or go stale.
- If a newer firmware version is available, a banner appears at the top — informational only; flashing is always your choice.

The other two ways into the same manual — the **Need guidance?** panel and the tappable warning banners — are covered in [Getting Help](../getting-help.md).

> Earlier firmware downloaded the manual over WiFi and could copy it to a microSD card with a green **"Save offline"** button. That button is gone in v1.3.2 and none of it is needed any more. An old copy left on a card is simply ignored — delete `/qmx-panadapter/manual/` if you want the space back.

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
