# Glossary

Common terms and acronyms used throughout this guide.

| Term | Meaning |
|------|---------|
| **ADIF** | Amateur Data Interchange Format — the standard log-file format for QSO records (exported by the panadapter, uploaded to QRZ/eQSL) |
| **CAT** | Computer-Aided Transceiver — radio control protocol (Kenwood-style commands via serial/USB) |
| **CDC-ACM** | Communications Device Class / Abstract Control Model — USB standard for serial ports |
| **CQ** | General call to any station (not directed at anyone specific) |
| **CW** | Continuous Wave — Morse code mode |
| **dBm** | Decibel-milliwatts — absolute signal power (0 dBm = 1 mW); used on the spectrum scale and S-meter |
| **DSP** | Digital Signal Processing — mathematical signal analysis and filtering |
| **eQSL** | Electronic QSL — online service for confirming and exchanging QSO records |
| **FFT** | Fast Fourier Transform — algorithm that converts time-domain audio into a frequency spectrum |
| **FT8 / FT4** | Digital modes for weak-signal HF communication (15-second vs 7.5-second slots) |
| **GFSK** | Gaussian Frequency-Shift Keying — the modulation scheme FT8 and FT4 use |
| **GPIO** | General-Purpose Input/Output — microcontroller pins for digital signals |
| **I2C / SPI** | Serial communication protocols for connecting peripherals (sensors, displays, etc.) |
| **IF** | Intermediate Frequency — the QMX presents the VFO signal at a +12 kHz offset in baseband |
| **IQ** | In-phase / Quadrature — stereo representation of RF signals (real + imaginary parts) |
| **LDPC** | Low-Density Parity-Check — the error-correcting code used in FT8/FT4 decoding |
| **LoTW** | Logbook of The World — ARRL's online QSO-confirmation service |
| **LSB / USB (mode)** | Lower / Upper Sideband — the two SSB voice modes (note: "USB" also means Universal Serial Bus, below) |
| **LVGL** | Light and Versatile Graphics Library — open-source embedded UI toolkit used for the display |
| **NVS** | Non-Volatile Storage — persistent memory on the ESP32 (survives power cycles) |
| **POTA** | Parks on the Air — portable operating activity from designated parks |
| **PSRAM** | Pseudo-SRAM — extra RAM on the Tab5 (used for large buffers like waterfall history) |
| **QMX / QMX+** | QRP Labs HF transceiver — the radio this panadapter controls and receives audio from |
| **QRZ** | QRZ.com Logbook — online logbook and callsign service for uploading QSOs |
| **QSO** | Radio contact / conversation between two stations |
| **RTC** | Real-Time Clock — battery-backed timer on the Tab5 (keeps time during power-off) |
| **SNR** | Signal-to-Noise Ratio — signal strength relative to the noise floor; the FT8/FT4 signal report |
| **SNTP** | Simple Network Time Protocol — synchronizes the system clock via WiFi/internet |
| **SOTA** | Summits on the Air — portable operating activity from mountain summits |
| **SSB** | Single Sideband — the voice-mode family (USB / LSB) |
| **STFT** | Short-Time Fourier Transform — sliding-window FFT used to build the waterfall |
| **SWR** | Standing Wave Ratio — antenna impedance matching metric (1.0 = perfect) |
| **TX / RX** | Transmit / Receive — keying the radio and listening |
| **UAC** | USB Audio Class — standard for streaming audio over USB |
| **USB** | Universal Serial Bus — physical connector and protocol (carries both audio and CAT commands). In a radio context, "USB" can also mean Upper Sideband — see SSB. |
| **UTC** | Coordinated Universal Time — timezone-independent time standard for FT8 slot alignment |
| **VFO** | Variable Frequency Oscillator — the radio's tuning dial / frequency setting |
