# Hardware Reference

## M5Stack Tab5

- **Processor**: ESP32-P4 @ 360 MHz
- **RAM**: 2 MB SRAM + 28 MB PSRAM
- **Storage**: 16 MB flash (firmware + settings)
- **Display**: 5" IPS LCD, 720×1280 landscape (10-bit parallel)
- **Touch**: ST7121 or ST7123 capacitive (100 kHz I2C)
- **Audio**: I2S codec ES8388 (CW audio output)
- **WiFi**: C6 co-processor (WiFi 6 802.11ax)
- **USB**: USB 2.0 host (USB-A port) + USB 2.0 device (USB-C port)
- **Battery**: 2500 mAh Li-poly (~4 hours panadapter, ~6 hours idle)
- **RTC**: Epson RX8130CE + 70 mF supercap (30–40 h time retention)
- **Optional Keyboard**: M5Stack SKU A164 (STM32F030, I2C slave @ 0x6D)

## QRP Labs QMX / QMX+

### USB Audio (UAC)

- **Interface**: USB Audio Class (no drivers needed)
- **Format**: Stereo I/Q, 48 kHz sample rate, 24-bit PCM
- **Latency**: ~50 ms (ring buffer)
- **I/Q Offset**: +12 kHz IF (VFO signal at +12 kHz in baseband)

### USB CAT (CDC-ACM)

- **Interface**: USB CDC-ACM (virtual serial port)
- **Baudrate**: 9600 (fixed)
- **Protocol**: Kenwood-style commands (FA, MD, FW, TX, RX, PC, SW, etc.)
- **Format**: Command; (terminated with semicolon)

### CAT Commands (Reference)

| Command | Query | Set | Notes |
|---|---|---|---|
| **FA** | `FA;` → `FA14074000;` | `FA14074000;` | Frequency (Hz) |
| **MD** | `MD;` → `MD2;` | `MD2;` | Mode (1=LSB, 2=USB, 3=CW, 5=DiGi) |
| **FW** | `FW;` → `FW2500;` | `FW2500;` | Filter width (Hz, SSB only) |
| **TX** | — | `TX;` | Key transmitter |
| **RX** | — | `RX;` | Release transmitter |
| **TA** | — | `TA14074000;` | Tune and key (FT8 burst) |
| **PC** | `PC;` → `PC05.0;` | `PC05.0;` | Power (watts, query only) |
| **SW** | `SW;` → `SW1.25;` | — | SWR (query while keyed) |
| **TM** | `TM;` → `TM123000;` | — | Time (hhmmss, query only) |
| **VN** | `VN;` → `VN1_03_002QMX;` | — | Firmware version (query only) |

### SSB Filter Control (Special)

The QMX menu-manager controls SSB filter via:

- **`MMSSB|Filter RX=<hz>;`** — commit filter (persists)
- **`MMSSB|Bandwidth=<hz>;`** — apply live filter
- Both must be set to the same value for a clean change

See [CLAUDE.md](https://github.com/SteffenLav/qmx-panadapter/blob/main/CLAUDE.md#ssb-filter-bandwidth-needs-three-coordinated-writes-the-hard-won-recipe) for the full recipe.

## Cables

### Tab5 ← QMX (USB Audio + CAT)

- **Type**: USB-A (Tab5 host) → USB-C (QMX)
- **Data lines**: Required (not charge-only)
- **Power**: Carries 5 V to QMX (optional; QMX can be separately powered)
- **Cable quality**: 3 m or less recommended (signal integrity)

### Tab5 ← Power

- **Type**: USB-C, 5 V DC
- **Current**: 2 A minimum (Tab5 draws ~2.5 A at full brightness + FT8 TX)
- **Source**: USB power adapter, power bank, or solar panel

## Frequency Ranges

The QMX/QMX+ operates across these HF bands:

| Band | Frequency | Notes |
|---|---|---|
| **160 m** | 1.8–2.0 MHz | Panadapter span: 1.8–2.0 MHz |
| **80 m** | 3.5–3.8 MHz | Panadapter span: 3.5–3.8 MHz |
| **40 m** | 7.0–7.3 MHz | Panadapter span: 7.0–7.3 MHz |
| **30 m** | 10.1–10.15 MHz | SSB not available (CW/DiGi only) |
| **20 m** | 14.0–14.35 MHz | Most FT8 activity |
| **17 m** | 18.068–18.168 MHz | — |
| **15 m** | 21.0–21.45 MHz | — |
| **12 m** | 24.89–24.99 MHz | — |
| **10 m** | 28.0–29.7 MHz | — |

The Tab5 panadapter shows a **4 MHz span** by default (tunable via zoom).

## Power Consumption

### Tab5 (estimated)

| State | Current | Duration (2500 mAh) |
|---|---|---|
| **Idle** (display off) | ~50 mA | 50 h |
| **Panadapter** (RX only, display on) | ~600 mA | 4 h |
| **FT8 RX** (decoding, display on) | ~700 mA | 3.5 h |
| **FT8 TX** (5 W burst) | ~1.5 A | 1.5 h (peak) |
| **WiFi + RX** | ~800 mA | 3 h |

### QMX+

| State | Current | Notes |
|---|---|---|
| **Standby** | ~50 mA | RX audio is off |
| **RX standby** | ~200 mA | RX active, no TX |
| **TX** (5 W) | ~2.5 A | Depends on antenna SWR |

Running both devices on battery:
- Tab5 2500 mAh + QMX 1500 mAh ≈ 2–3 hours of active FT8 operation

## Antennas

The QMX/QMX+ uses a **50 Ω coaxial antenna connector** (SMA or BNC depending on model).

Recommended antennas:
- **Dipole** (1/2 wave) — best all-round performance
- **EFHW** (end-fed half-wave) — portable, single-wire
- **Vertical** (1/4 wave + radials) — omnidirectional
- **Magnetic loop** (small, tuned) — RFI rejection, low noise

Maximum power: **5 W (QMX+) or 2.5 W (QMX)** — a good antenna is critical at these power levels.

---

**Next:** Review the [Web API](web-api.md) or troubleshoot via [Troubleshooting](troubleshooting.md).
