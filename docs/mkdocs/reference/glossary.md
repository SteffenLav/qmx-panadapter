# Glossary

Common terms and acronyms used throughout this guide.

| Term | Meaning |
|------|---------|
| **ADIF** | Amateur Data Interchange Format — the standard log-file format for QSO records (exported by the panadapter, uploaded to QRZ/eQSL) |
| **AM** | Amplitude Modulation — a voice mode. Receive only on the QMX+, and only on firmware 1_04 and later |
| **ARRL** | American Radio Relay League — the US national amateur radio society, which runs Logbook of The World |
| **CAT** | Computer-Aided Transceiver — radio control protocol (Kenwood-style commands via serial/USB) |
| **CDC-ACM** | Communications Device Class / Abstract Control Model — USB standard for serial ports |
| **CQ** | General call to any station (not directed at anyone specific) |
| **CW** | Continuous Wave — Morse code mode |
| **dBm** | Decibel-milliwatts — absolute signal power (0 dBm = 1 mW); used on the spectrum scale and S-meter |
| **Directed message** | A JS8 message aimed at one station, shown with the sender's callsign |
| **DSP** | Digital Signal Processing — mathematical signal analysis and filtering |
| **DT** | In the decode list, how far a station's transmission sits from the slot boundary, in seconds. It measures THEIR clock against yours, so it stays blank until at least two separate stations have been heard |
| **DX** | A distant station, or distance worked. **BEST DX** on the WSPR page is the furthest station that heard you, or that you heard, this session |
| **DXCC** | DX Century Club — the ARRL award programme, and by extension its list of ~340 "entities" (countries plus separately-counted islands and territories) |
| **eQSL** | Electronic QSL — online service for confirming and exchanging QSO records |
| **FFT** | Fast Fourier Transform — algorithm that converts time-domain audio into a frequency spectrum |
| **Free text** | A plain typed message rather than a fixed exchange. JS8 carries it; FT8 and FT4 fit only 13 characters |
| **FT8 / FT4** | Digital modes for weak-signal HF contacts, trading a short fixed exchange in 15-second (FT8) or 7.5-second (FT4) slots. Both decode and transmit on the device. |
| **GFSK** | Gaussian Frequency-Shift Keying — the modulation FT8, FT4 and JS8 all use |
| **GNSS / GPS** | A satellite navigation receiver, which gives the Tab5 UTC and the date with no internet - an M5Stack Unit GPS on PORT.A, a Module GPS on the 30-pin bus, or a GPS-equipped QMX+ answering over CAT |
| **GPIO** | General-Purpose Input/Output — microcontroller pins for digital signals |
| **Heartbeat** | The periodic `HB <call> <grid>` a JS8 station sends to say it is listening |
| **I2C / SPI** | Serial communication protocols for connecting peripherals (sensors, displays, etc.) |
| **IF** | Intermediate Frequency — the QMX presents the VFO signal at a +12 kHz offset in baseband |
| **IQ** | In-phase / Quadrature — stereo representation of RF signals (real + imaginary parts) |
| **JS8** | A keyboard mode built on FT8's modulation that carries free text, so stations hold short conversations. Decoded and transmitted on the device, as a sub-mode of the FT8/FT4 page |
| **LDPC** | Low-Density Parity-Check — the error-correcting code that recovers a damaged FT8, FT4 or JS8 message |
| **LoTW** | Logbook of The World — ARRL's online QSO-confirmation service |
| **LSB / USB (mode)** | Lower / Upper Sideband — the two SSB voice modes (note: "USB" also means Universal Serial Bus, below) |
| **LVGL** | Light and Versatile Graphics Library — open-source embedded UI toolkit used for the display |
| **Maidenhead / grid** | The locator system amateurs use for position, e.g. `JO65`. Four characters is a ~100 km square, six is finer. FT8 and WSPR both carry one |
| **NMEA** | The sentence format a GNSS receiver speaks - what the Tab5 reads for time, date and position |
| **NVS** | Non-Volatile Storage — persistent memory on the ESP32 (survives power cycles) |
| **PA** | Power Amplifier — the radio's final transmit stage. **Max. PA voltage** sets how hard it is driven, and so how much power comes out; see [WSPR](../guide/wspr.md) and Calibrate Power |
| **OTA** | Over-The-Air update - the Tab5 fetching and installing new firmware over WiFi, with no cable |
| **Pileup** | Everyone calling you at once. The Tab5 collects them in a list so you can work them in turn |
| **POTA** | Parks on the Air — portable operating activity from designated parks |
| **PSK Reporter** | A worldwide database of who heard whom. The panadapter can send it your decodes, and reads it back to show who has heard **you** |
| **PSRAM** | Pseudo-SRAM — extra RAM on the Tab5 (used for large buffers like waterfall history) |
| **QMX / QMX+** | QRP Labs HF transceiver — the radio this panadapter controls and receives audio from |
| **QRP** | Low-power operating, conventionally 5 W or less. QRP Labs, who make the QMX, are named for it |
| **QRZ** | QRZ.com Logbook — online logbook and callsign service for uploading QSOs |
| **QSO** | Radio contact / conversation between two stations |
| **RBN** | Reverse Beacon Network — automated receivers ("skimmers") that continuously report the CW and digital signals they hear. One of SelfSpotter's three sources |
| **RIT** | Receiver Incremental Tuning — shifts what you *hear* without moving what you would *transmit* on. The QMX has RIT but no XIT |
| **RTC** | Real-Time Clock — battery-backed timer on the Tab5 (keeps time during power-off) |
| **Slot** | The fixed window a digital mode transmits in - 15 s for FT8 and JS8, 7.5 s for FT4. Every station starts together, which is why the clock must be right |
| **SNR** | Signal-to-Noise Ratio — signal strength relative to the noise floor; the FT8/FT4 signal report |
| **SNTP** | Simple Network Time Protocol — synchronizes the system clock via WiFi/internet |
| **SOTA** | Summits on the Air — portable operating activity from mountain summits |
| **SSB** | Single Sideband — the voice-mode family (USB / LSB) |
| **SSID** | The name of a WiFi network |
| **STFT** | Short-Time Fourier Transform — sliding-window FFT used to build the waterfall |
| **SWR** | Standing Wave Ratio — antenna impedance matching metric (1.0 = perfect) |
| **TX / RX** | Transmit / Receive — keying the radio and listening |
| **UAC** | USB Audio Class — standard for streaming audio over USB |
| **USB** | Universal Serial Bus — physical connector and protocol (carries both audio and CAT commands). In a radio context, "USB" can also mean Upper Sideband — see SSB. |
| **UTC** | Coordinated Universal Time — timezone-independent time standard for FT8 slot alignment |
| **VFO** | Variable Frequency Oscillator — the radio's tuning dial / frequency setting |
| **WSJT-X** | The desktop program most FT8 and WSPR operators use. The panadapter does the same job without a PC, and its decodes and reports are meant to be comparable |
| **WSPR** | Weak Signal Propagation Reporter, said "whisper" — a beacon mode carrying only callsign, grid and power. Nobody replies; stations worldwide report hearing you, so you learn where your signal actually goes. See [WSPR](../guide/wspr.md) |
