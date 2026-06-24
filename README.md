# XIAO ESP32-C6 × MAX30102 — Heart Rate / SpO₂ / HRV over BLE

A tiny wearable-style PPG monitor: a **Seeed XIAO ESP32-C6** streams raw photoplethysmography (IR/RED) samples over **Bluetooth LE**, and a **single self-contained web page** (Web Bluetooth) does *all* the signal processing — beat detection, RR intervals, time-domain HRV, and SpO₂ — while visualizing the live pulse waveform.

> ⚠️ For learning / hobby use only. PPG gives **PRV** (pulse-rate variability, an approximation of HRV) and is **not a medical device**.

---

## Architecture

```
  MAX30102  ──I²C──►  XIAO ESP32-C6  ──BLE (raw IR/RED @100Hz)──►  Browser (Chrome/Edge)
 (PPG sensor)         (firmware: read FIFO,                       (index.html: DC removal,
                       batch, BLE notify —                         peak detection, RR, HRV,
                       NO algorithms)                              SpO₂, live PPG + charts)
```

The firmware is intentionally "dumb" — it only ships raw samples. **All algorithms live in the browser** in plain JavaScript, so you can tune the beat detector, change HRV math, or add metrics by editing one HTML file — **no reflashing required**. The computer also has none of the MCU's compute limits.

---

## Hardware & Wiring (I²C, 4 wires)

| MAX30102 | XIAO ESP32-C6 | Note |
|---|---|---|
| VIN | **3V3** | not 5V/VBUS |
| GND | GND | common ground |
| SDA | **D4** (GPIO22) | |
| SCL | **D5** (GPIO23) | |
| INT | — | unused (polling) |

I²C address `0x57`. If the sensor isn't detected, the usual cause is **SDA/SCL swapped** — try swapping them.

---

## Firmware — build & flash

Uses [`arduino-cli`](https://arduino.github.io/arduino-cli/) with the Espressif ESP32 core.

```bash
# one-time setup
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "SparkFun MAX3010x Pulse and Proximity Sensor Library"

# compile + upload (replace PORT)
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6 -u -p /dev/cu.usbmodemXXXX max30102_ble
```

The board advertises as **`XIAO-HR`**. Serial @115200 prints `sps=.. idx=.. IR=.. conn=..` for bench checks (no finger needed).

### BLE protocol

- Service `12345678-1234-5678-1234-56789abcdef0`, notify characteristic `…def1`
- Each notification is a little-endian binary packet:
  `[u32 firstIndex][u8 count][ count × (u32 ir, u32 red) ]`
- Effective sample rate **100 Hz**; the browser reconstructs the timeline from the absolute sample index.

---

## Web app — usage

`web/index.html` is a **single file, no dependencies, no install**.

1. Open it in **Chrome** or **Edge** (Windows / macOS / Linux). *Safari & Firefox don't support Web Bluetooth.*
2. Click **接続 (Connect)** → pick **XIAO-HR**.
3. Rest a fingertip gently on the optical window and hold still.

You'll see the live **PPG waveform** (green dots = detected beats), **heart rate**, **SpO₂**, and time-domain HRV: **RMSSD, pNN50, SDNN**, a **Poincaré plot** (SD1/SD2), an **RR tachogram**, and a relaxation indicator. The page auto-reconnects if BLE drops, and a log panel + CSV export are included. (UI is in Japanese.)

Beat-detector parameters are exposed at the top of the script (`const P = {...}`) for easy tuning.

---

## Repository layout

| Path | What |
|---|---|
| `max30102_ble/` | **Main firmware** — raw IR/RED BLE streamer |
| `web/index.html` | **Web app** — all processing + visualization |
| `i2c_diag/` | Standalone I²C bus diagnostic (scans the bus, reports line levels) — handy when wiring |
| `max30102_xiao/` | Early serial-only heart-rate demo (reference) |

---

## 中文说明

XIAO ESP32-C6 通过 I²C 读取 MAX30102 光电脉搏传感器,把**原始 IR/RED 样本**经 **BLE** 以 100Hz 流给浏览器;**网页(单文件,Web Bluetooth)负责全部算法**:去直流、心跳峰值检测、RR、时域 HRV(RMSSD/pNN50/SDNN、Poincaré、リラックス度)、R 比值血氧,并显示实时脉搏波。

固件只管"传原始数据"、不含算法 —— 改算法只改网页、**无需重新烧录**,也绕开了 MCU 算力/无 FPU 的限制。跨平台(Win/Mac/Linux 的 Chrome/Edge),免驱动、免串口、免安装。**仅供学习,非医疗用途。**

接线见上表(VIN→3V3, GND→GND, SDA→D4, SCL→D5)。若 `0x57` 认不到,多半是 SDA/SCL 接反,对调即可。

---

## License

MIT — see [LICENSE](LICENSE).
