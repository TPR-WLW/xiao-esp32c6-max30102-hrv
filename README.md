# XIAO ESP32-C6 + MAX30102: pulse, SpO₂ and HRV over BLE

> English | [中文](README.zh-CN.md) | [日本語](README.ja.md)

A small heart-rate, SpO₂ and HRV monitor. The board reads an optical pulse
sensor and streams the raw light over Bluetooth. A web page does everything else.

I built it because I wanted to watch my own heart-rate variability without buying
a chest strap, and because I was curious how far a five-dollar sensor and a
browser could go.

## The idea

The device is dumb on purpose. The ESP32 reads the MAX30102 and pushes raw
IR/RED samples over BLE at 100 Hz. No filtering, no beat detection, no math on
the chip.

Everything that turns those numbers into something meaningful runs in the
browser, in plain JavaScript: beat detection, the spacing between beats, HRV,
SpO₂, and the live waveform.

Two reasons I split it this way:

- The C6 has no hardware floating-point unit. Anything heavier than integer math
  runs in software emulation, which is slow, and with the BLE stack fighting for
  the same single core it was occasionally enough to stall the loop. (That cost
  me an evening. The firmware now does as little as possible and the problem went
  away.)
- When the algorithms live in the page, changing one is a text edit. I got tired
  of reflashing every time I wanted to nudge the beat detector. Now I save the
  HTML and refresh.

It runs the same on Windows, macOS and Linux as long as you have Chrome or Edge,
with nothing to install.

It's a learning project, not a medical device. A MAX30102 measures pulse-rate
variability (PRV). At rest that tracks ECG-derived HRV fairly well; during
movement it doesn't, and the absolute numbers shouldn't be taken too seriously.

## Hardware

- Seeed XIAO ESP32-C6
- MAX30102 breakout (the common purple GY-MAX30102 works)
- Four jumper wires

Wiring (I²C):

| MAX30102 | XIAO ESP32-C6 |
|----------|---------------|
| VIN      | 3V3           |
| GND      | GND           |
| SDA      | D4 (GPIO22)   |
| SCL      | D5 (GPIO23)   |

Power from 3V3, not 5V. The sensor sits at I²C address 0x57. If a scan finds
nothing, check whether SDA and SCL are swapped before anything else. They were,
for me, and the symptom is maddening: the pull-ups read fine, the bus looks
healthy, and the chip just never answers.

## Firmware

Built with arduino-cli and the Espressif ESP32 core.

```bash
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "SparkFun MAX3010x Pulse and Proximity Sensor Library"

# compile and upload; use your own serial port
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6 -u \
  -p /dev/cu.usbmodemXXXX max30102_ble
```

The MAX30102 is set to Red+IR at a 400 Hz internal rate with 4× averaging, so it
delivers 100 effective samples a second. The firmware drains the sensor FIFO,
packs samples into small batches, and notifies. It keeps no history and runs no
DSP; if you read the loop it's almost boring, which is the point.

It advertises as `XIAO-HR`. Over USB serial at 115200 it prints one line a second
(sample rate, running sample index, last IR value, whether a central is
connected), so you can confirm it's alive without a finger on the sensor.

### BLE format

One service, one notify characteristic. Each notification is a little-endian
binary packet:

```
[uint32 firstIndex][uint8 count][ count × (uint32 ir, uint32 red) ]
```

- `firstIndex` is the absolute sample number of the first sample. The browser
  uses it to rebuild the timeline and to notice a dropped packet.
- `count` is 16 in this build, so about six notifications a second. Bigger
  batches mean fewer notifications and less load on the radio, while staying
  small enough to fit a normally negotiated MTU.
- The rate is fixed at 100 Hz, which the browser hard-codes to turn sample
  indices into time.

Service `12345678-1234-5678-1234-56789abcdef0`, characteristic `…def1`.

## The web app

`web/index.html` is one file: no dependencies, no build, no server. Double-click
it. Use Chrome or Edge; Safari and Firefox don't implement Web Bluetooth.

Connect, pick `XIAO-HR`, rest a fingertip on the window and hold still. After a
few seconds you get the pulse waveform with a dot on each detected beat, heart
rate, SpO₂, and the HRV panel. It speaks English, Chinese and Japanese, logs
every packet, exports CSV, and reconnects on its own if the link drops.

What happens under the hood:

- **Beat detection.** The raw IR is high-passed by subtracting a slow exponential
  moving average (about a two-second time constant), which removes baseline drift
  without flattening the pulse; get that time constant wrong and you detect
  nothing. A light low-pass smooths the rest. Peaks are found against an adaptive
  threshold from a decaying envelope, with a refractory period so the dicrotic
  notch isn't counted as a second beat.
- **RR timing.** Each peak is refined with a parabolic fit over its three
  neighbouring samples, which pushes the interval resolution below the 10 ms
  sample spacing. RR is measured from sample indices, not wall-clock time, so
  Bluetooth jitter doesn't smear it.
- **HRV.** Time-domain only: mean RR, RMSSD, pNN50, SDNN, plus SD1/SD2 from the
  Poincaré cloud. Intervals more than 30% off the local median are dropped first,
  so a single missed or doubled beat doesn't wreck the numbers.
- **SpO₂.** The usual ratio-of-ratios, R = (AC_red/DC_red) / (AC_ir/DC_ir) over a
  few seconds, mapped with the standard linear approximation and clamped. It's
  uncalibrated, so treat it as a ballpark.

The beat-detector constants are in one object at the top of the script
(`const P`). Since it's all in the page, tuning is edit-and-refresh.

## Layout

```
max30102_ble/    firmware — streams raw IR/RED over BLE
web/index.html   the web app
i2c_diag/        a standalone I²C scanner I used while wiring
max30102_xiao/   the first version, serial only, kept for reference
```

## License

MIT.
