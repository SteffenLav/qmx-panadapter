# M5Tab5 Keyboard — reference code & integration notes

Reference copy of M5Stack's official keyboard demo, fetched 2026-06-19 from
<https://github.com/m5stack/M5Tab5-Keyboard-UserDemo> (MIT). This is the SKU **A164**
70-key keyboard accessory for the Tab5. Found via John Hendry on the QRPLabs groups.io thread.

**This is reference only — not wired into the build.** The driver here is C++/ESP-IDF
(`namespace m5`, the new `i2c_master_*` API); our project is C. Port the protocol, don't
drop the component in as-is.

## Why the earlier attempt failed

We previously probed a **TCA8418** matrix-encoder at **I2C 0x34** (see the old
`project_tab5_keyboard` memory). That was the wrong chip and the wrong address. The
keyboard is actually an **STM32F030C8T6 acting as an I2C slave** with its own register
map. Nothing answers at 0x34 — that's why contact was never established.

## Hardware / bus facts

| | |
|---|---|
| Controller | STM32F030C8T6 (I2C slave, custom register map) |
| I2C address | **0x6D** (7-bit) |
| SDA / SCL | GPIO **0** / GPIO **1** |
| INT | GPIO **50** (active-low, negedge; pull-up enabled) |
| I2C speed | 100 kHz (default) or 400 kHz — both valid |
| Matrix | 5 rows × 14 cols (70 keys) |

GPIO 50 matching our old note is a coincidence — that pin is right, the chip/address were not.

> ⚠️ Bus sharing: GPIO0/GPIO1 is M5Stack's **internal system I2C** on the Tab5, the same
> bus the PI4IO expander / RTC / etc. live on. Confirm whether the keyboard sits on the
> shared `sys_i2c` bus (`components/.../m5tab5_sys_i2c.cpp` upstream) before adding a new
> master — reuse the existing bus handle rather than creating a second master on the same pins.

## Register map (from `m5_tab5_keyboard.h`)

| Reg | Name | Meaning |
|-----|------|---------|
| 0x00 | INT_CFG | interrupt config (also the "is it alive?" probe register) |
| 0x01 | INT_STA | interrupt status; bit0=Normal, bit1=HID, bit2=String event pending |
| 0x02 | EVENT_NUM | number of queued events; write 0 to clear queue |
| 0x03 | BRIGHTNESS | backlight 0–100 |
| 0x10 | KEYBOARD_MODE | 0=Normal, 1=HID, 2=String, 3=BLE |
| 0x11 | RGB_MODE | 0=binding, 1=custom |
| 0x20 | KEY_EVENT | one Normal-mode key event byte (see below) |
| 0x30 | HID_EVENT | 2 bytes: [modifier, keycode] |
| 0x40 | CHAR_EVENT_LEN | String-mode pending length |
| 0x50 | CHAR_EVENT_BASE | String-mode payload: [modifier, char0, char1, …] |
| 0x60 | RGB_COLOR_BASE | per-key RGB (B,G,R order) |
| 0xFE | VERSION | firmware version byte |
| 0xFF | I2C_ADDR | change slave address |

Read protocol is plain register read: write the register byte (repeated-start, no stop),
then read N bytes. See `m5_tab5_keyboard_i2c_compat.h`.

### Normal-mode KEY_EVENT byte (reg 0x20)
```
bit7      = pressed (1) / released (0)
bits6..4  = row  (0–4)
bits3..0  = col  (0–13)
```
`0xFF` = no event. A row/col → character lookup table lives in the LVGL wrapper
(`lvgl_wrapper_m5tab5_keyboard/`) — that's the layer that turns row/col into ASCII.

## Recommended integration path for the panadapter

**Use String mode (mode 2), not Normal mode.** In String mode the STM32 does the keymap,
shift/modifier handling, and debouncing itself, and hands back ready ASCII in `str_data`.
That sidesteps having to maintain our own row/col→char table (the thing the old memory
flagged as an unverified placeholder). For a panadapter we just want typed characters into
a textarea (callsign, WiFi password, frequency entry), so String mode is the natural fit.

Minimal C port:
1. Add the keyboard as a device on the existing system I2C bus at 0x6D, 100 kHz.
2. Probe reg 0x00 — if it ACKs, the keyboard is attached (hot-plug friendly: re-probe).
3. Write reg 0x10 = 2 (String mode); clear EVENT_NUM (0x02←0) and INT_STA (0x01←0).
4. Poll reg 0x01 every ~50 ms (or use the GPIO50 negedge IRQ). If bit2 set:
   read EVENT_NUM (0x02), then for each event read LEN (0x40) and LEN+1 bytes from 0x50
   ([modifier, chars…]); feed the chars to the focused LVGL textarea; write 0x01←0 to clear.

The full read/decode loop is `_handleInterrupt()` in `m5_tab5_keyboard.cpp` — copy that logic.
