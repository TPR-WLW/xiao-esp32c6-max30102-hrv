# XIAO ESP32-C6 + MAX30102 — BLE PPG streamer (technical reference)

> English | [日本語](README.ja.md)

Raw photoplethysmography (PPG) acquisition on a Seeed XIAO ESP32-C6 with a
MAX30102, streamed over BLE. All signal processing — beat detection, RR
intervals, time-domain HRV, SpO₂ — runs in a browser over Web Bluetooth. This is
the technical reference. (Backup mirror; EN/JA only.)

## System overview

```
MAX30102 --I2C(0x57,400kHz)--> XIAO ESP32-C6 --BLE GATT notify--> Browser (Chrome/Edge)
  Red+IR PPG                    FIFO drain + batch                DSP: detect/RR/HRV/SpO2 + render
```

The MCU runs no algorithms by design. The ESP32-C6 (RISC-V, single core) has no
FPU, so floating-point DSP is software-emulated and slow, and it contends with
the BLE host for the core. Moving all computation to the client removes that
limit and lets algorithms change without reflashing.

Not a medical device. A MAX30102 yields pulse-rate variability (PRV), an
approximation of ECG-derived HRV that holds mainly at rest.

## Hardware / wiring

| MAX30102 | XIAO ESP32-C6 | |
|----------|---------------|--|
| VIN | 3V3 | sensor is 3.3 V; do not use 5 V |
| GND | GND | |
| SDA | D4 / GPIO22 | I²C data |
| SCL | D5 / GPIO23 | I²C clock |

I²C address `0x57`, bus at 400 kHz. A scan that returns nothing while the
pull-ups read healthy almost always means SDA and SCL are swapped.

## Sensor configuration

SparkFun `MAX30105` driver, initialised as:

| Parameter | Value | Note |
|-----------|-------|------|
| LED mode | 2 | Red + IR |
| Sample rate | 400 Hz | internal |
| Sample averaging | 4 | → 100 Hz effective |
| Pulse width | 411 µs | 18-bit ADC |
| ADC range | 4096 nA | |
| LED amplitude (Red, IR) | 0x24 | ≈ 7 mA |

Effective output rate = 400 / 4 = **100 Hz**. The firmware drains the FIFO with
`getFIFOIR()` / `getFIFORed()` (tail reads) and `nextSample()`. No filtering or
detection on the device.

## BLE GATT

| | UUID |
|--|------|
| Service | `12345678-1234-5678-1234-56789abcdef0` |
| Characteristic (notify) | `12345678-1234-5678-1234-56789abcdef1` |

Device name `XIAO-HR`. Requested ATT MTU 247. Each notification carries a batch
of samples.

### Packet layout (little-endian)

| Offset | Type | Field |
|--------|------|-------|
| 0 | uint32 | `firstIndex` — absolute sample number of sample 0 in this packet |
| 4 | uint8 | `count` — sample count (16 in this build) |
| 5 + 8k | uint32 | `ir[k]` |
| 9 + 8k | uint32 | `red[k]` |

Packet size = 5 + 8·count = **133 bytes** at count = 16. Notify rate ≈ 100 / 16
≈ **6.25 Hz**. `firstIndex` lets the client order packets and detect loss; the
100 Hz sample rate is fixed and assumed client-side.

A stall watchdog re-initialises the sensor only after >5 s without samples (long
enough not to trip on BLE contention) and resets no detector state — detection
lives on the client.

Serial debug at 115200 baud prints one line per second: `sps`, sample index,
last IR value, connection flag.

## Client processing (`web/index.html`, vanilla JS)

All tunables are in `const P` at the top of the script.

**Beat detection, per IR sample:**

- DC estimate: `dc += (ir − dc)·α`, α = 0.005, so τ ≈ 1/(α·fs) ≈ 2 s — a ~0.08 Hz
  high-pass, slow enough not to attenuate the pulse.
- AC: `ac = ir − dc`, then low-pass `acLP += (ac − acLP)·0.4`.
- Envelope: `envPos` / `envNeg` decay ×0.995 per sample, tracking max/min of
  `acLP`. Threshold = `envNeg + 0.55·(envPos − envNeg)`; minimum peak-to-peak
  amplitude 200.
- Peak: a local maximum of `acLP` above threshold, refined to sub-sample position
  by parabolic interpolation of its three neighbours:
  `offset = 0.5·(y₋₁ − y₊₁) / (y₋₁ − 2y₀ + y₊₁)`.
- Refractory 300 ms; accepted RR ∈ [300, 2000] ms (30–200 BPM).

**RR / HRV:**

- `RR = (peakIndexᵢ − peakIndexᵢ₋₁) / fs · 1000` ms, derived from sample indices
  so BLE jitter does not affect it.
- Artifact rejection: drop RR if `|RR − median(last 11)| > 0.30 · median`.
- Sliding window of the last 60 intervals: mean RR, SDNN, RMSSD, pNN50; Poincaré
  `SD1 = √0.5 · SDSD`, `SD2 = √(2·SDNN² − SD1²)`.

**SpO₂ (ratio-of-ratios):**

- Window ≈ 3 s (300 samples). `DC = mean`, `AC = RMS(x − DC)` for Red and IR.
- `R = (AC_red/DC_red) / (AC_ir/DC_ir)`, `SpO₂ = clamp(110 − 25R, 70, 100)`.
  Requires `DC_ir > 50000` (finger present) and `AC > 20`. Uncalibrated.

Finger gate `IR > 50000`. The client auto-reconnects (1.5 s retry) on
`gattserverdisconnected`.

## Build & flash

```bash
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "SparkFun MAX3010x Pulse and Proximity Sensor Library"
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6 -u -p <PORT> max30102_ble
```

Open `web/index.html` in Chrome or Edge (Web Bluetooth required), connect to
`XIAO-HR`, and place a fingertip on the sensor window.

## Repository

```
max30102_ble/    firmware — raw IR/RED BLE streamer
web/index.html   client — Web Bluetooth + all DSP
i2c_diag/        I²C bus scanner / line-level probe
max30102_xiao/   serial-only heart-rate prototype
```

## License

MIT.
