# XIAO ESP32-C6 + MAX30102 — BLE PPG ストリーマ（技術リファレンス）

> [English](README.md) | 日本語

Seeed XIAO ESP32-C6 と MAX30102 による生 PPG（光電容積脈波）取得。BLE で送信し、信号処理
（拍検出・RR 間隔・時間領域 HRV・SpO₂）はすべて Web Bluetooth 経由でブラウザ側が行う。本書は
技術リファレンス。（バックアップミラー、EN/JA のみ）

## 構成

```
MAX30102 --I2C(0x57,400kHz)--> XIAO ESP32-C6 --BLE GATT notify--> ブラウザ (Chrome/Edge)
  Red+IR PPG                    FIFO 吸い出し + バッチ化          DSP: 検出/RR/HRV/SpO2 + 描画
```

MCU 側はアルゴリズムを持たない設計。ESP32-C6（RISC-V、シングルコア）は FPU を持たず、浮動
小数点の DSP はソフトウェアエミュレーションで遅く、BLE ホストとコアを奪い合う。計算をすべ
てクライアントへ移すことで、この制約を外し、アルゴリズム変更を書き込み不要にする。

医療機器ではない。MAX30102 が与えるのは脈拍変動（PRV）で、心電由来の HRV の近似として有効
なのは主に安静時。

## ハードウェア / 配線

| MAX30102 | XIAO ESP32-C6 | |
|----------|---------------|--|
| VIN | 3V3 | センサは 3.3 V 動作。5 V は不可 |
| GND | GND | |
| SDA | D4 / GPIO22 | I²C データ |
| SCL | D5 / GPIO23 | I²C クロック |

I²C アドレス `0x57`、バス 400 kHz。プルアップが正常なのにスキャンで何も出ないときは、ほぼ
SDA と SCL の入れ替わり。

## センサ設定

SparkFun `MAX30105` ドライバを以下で初期化：

| パラメータ | 値 | 備考 |
|-----------|----|------|
| LED モード | 2 | Red + IR |
| サンプルレート | 400 Hz | 内部 |
| サンプル平均 | 4 | → 実効 100 Hz |
| パルス幅 | 411 µs | 18-bit ADC |
| ADC レンジ | 4096 nA | |
| LED 振幅 (Red, IR) | 0x24 | 約 7 mA |

実効出力レート = 400 / 4 = **100 Hz**。ファームウェアは `getFIFOIR()` / `getFIFORed()`
（tail 読み出し）と `nextSample()` で FIFO を吸い出す。デバイス側でのフィルタ・検出は一切
なし。

## BLE GATT

| | UUID |
|--|------|
| サービス | `12345678-1234-5678-1234-56789abcdef0` |
| キャラクタリスティック (notify) | `12345678-1234-5678-1234-56789abcdef1` |

デバイス名 `XIAO-HR`。要求 ATT MTU は 247。各通知はサンプルをバッチ化して運ぶ。

### パケット構造（リトルエンディアン）

| オフセット | 型 | フィールド |
|-----------|----|-----------|
| 0 | uint32 | `firstIndex` — 本パケット先頭サンプルの絶対番号 |
| 4 | uint8 | `count` — サンプル数（本ビルドでは 16） |
| 5 + 8k | uint32 | `ir[k]` |
| 9 + 8k | uint32 | `red[k]` |

パケット長 = 5 + 8·count = count = 16 で **133 バイト**。通知レート ≈ 100 / 16 ≈
**6.25 Hz**。`firstIndex` でパケットの順序付けと欠落検出を行う。サンプルレート（100 Hz）は
固定で、クライアントが前提とする。

ストール監視は >5 秒サンプルが来ないときだけセンサを再初期化する（BLE 競合では発火しない
長さ）。検出器の状態は一切リセットしない — 検出はクライアント側にある。

シリアルデバッグは 115200 baud で 1 秒ごとに 1 行（`sps`、サンプル番号、直近 IR、接続フラグ）。

## クライアント処理（`web/index.html`、素の JS）

調整値はスクリプト先頭の `const P` にまとまっている。

**拍検出（IR サンプルごと）：**

- DC 推定：`dc += (ir − dc)·α`、α = 0.005 なので τ ≈ 1/(α·fs) ≈ 2 秒 — 約 0.08 Hz のハイ
  パスで、脈を減衰させない程度に遅い。
- AC：`ac = ir − dc`、続いてローパス `acLP += (ac − acLP)·0.4`。
- 包絡：`envPos` / `envNeg` を毎サンプル ×0.995 で減衰させ `acLP` の最大/最小を追う。しきい
  値 = `envNeg + 0.55·(envPos − envNeg)`、最小 peak-to-peak 振幅 200。
- ピーク：しきい値を超える `acLP` の極大。ピーク前後 3 点の放物線補間でサブサンプル位置を
  求める：`offset = 0.5·(y₋₁ − y₊₁) / (y₋₁ − 2y₀ + y₊₁)`。
- 不応期 300 ms、採用する RR は [300, 2000] ms（30–200 BPM）。

**RR / HRV：**

- `RR = (peakIndexᵢ − peakIndexᵢ₋₁) / fs · 1000` ms。サンプル番号から算出するので BLE ジッ
  タの影響を受けない。
- アーティファクト除去：`|RR − median(直近 11)| > 0.30 · median` の RR を捨てる。
- 直近 60 区間のスライディングウィンドウで：平均 RR、SDNN、RMSSD、pNN50。Poincaré は
  `SD1 = √0.5 · SDSD`、`SD2 = √(2·SDNN² − SD1²)`。

**SpO₂（比の比）：**

- ウィンドウ ≈ 3 秒（300 サンプル）。Red と IR について `DC = 平均`、`AC = RMS(x − DC)`。
- `R = (AC_red/DC_red) / (AC_ir/DC_ir)`、`SpO₂ = clamp(110 − 25R, 70, 100)`。`DC_ir > 50000`
  （指あり）かつ `AC > 20` を要件とする。未校正。

指の判定は `IR > 50000`。クライアントは `gattserverdisconnected` で自動再接続（1.5 秒間隔
リトライ）。

## ビルドと書き込み

```bash
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "SparkFun MAX3010x Pulse and Proximity Sensor Library"
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6 -u -p <PORT> max30102_ble
```

`web/index.html` を Chrome か Edge で開き（Web Bluetooth 必須）、`XIAO-HR` に接続して指先
をセンサ窓に当てる。

## リポジトリ構成

```
max30102_ble/    ファームウェア — 生 IR/RED の BLE ストリーマ
web/index.html   クライアント — Web Bluetooth + 全 DSP
i2c_diag/        I²C バススキャナ / ライン電圧プローブ
max30102_xiao/   シリアル専用の心拍プロトタイプ
```

## ライセンス

MIT。
