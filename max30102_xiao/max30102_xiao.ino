/*
 * MAX30102 Heart-Rate / SpO2 demo  —  Seeed XIAO ESP32-C6
 * ------------------------------------------------------------------
 * I2C wiring (4 wires):
 *   MAX30102 VIN (UIN) -> XIAO 3V3      (NOT 5V / VBUS!)
 *   MAX30102 GND       -> XIAO GND
 *   MAX30102 SDA       -> XIAO D4  (GPIO22)
 *   MAX30102 SCL       -> XIAO D5  (GPIO23)
 *   MAX30102 INT       -> (optional) XIAO D2 (GPIO2)  — unused in polling mode
 *   MAX30102 RD / IRD  -> leave floating
 *
 * I2C address: 0x57 (fixed)
 * Library:     SparkFun MAX3010x  (header is MAX30105.h — also drives the MAX30102)
 *
 * What it does:
 *   1. Scans the I2C bus at boot and reports every address found.
 *   2. If the sensor answers, streams IR value + live BPM over Serial @115200.
 *   3. Place a fingertip gently on the optical window to get a reading.
 * ------------------------------------------------------------------
 */

#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

// ---- XIAO ESP32-C6 I2C pins ----
static const int PIN_SDA = 22;  // D4
static const int PIN_SCL = 23;  // D5

MAX30105 particleSensor;

// ---- Beat-averaging state ----
const byte RATE_SIZE = 8;       // number of beats to average
byte  rates[RATE_SIZE];         // ring buffer of recent BPM values
byte  rateSpot = 0;
long  lastBeat = 0;             // time (ms) of the previous detected beat
float beatsPerMinute = 0;
int   beatAvg = 0;

// ---- Finger-presence threshold on the IR channel ----
const long FINGER_THRESHOLD = 50000;

void i2cScan() {
  Serial.println(F("\n[I2C] scanning bus..."));
  byte found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("[I2C]   device @ 0x"));
      if (addr < 16) Serial.print('0');
      Serial.print(addr, HEX);
      if (addr == 0x57) Serial.print(F("  <- MAX30102"));
      Serial.println();
      found++;
    }
  }
  if (found == 0)
    Serial.println(F("[I2C]   no devices found — check VIN=3V3, GND, SDA=D4, SCL=D5"));
  Serial.print(F("[I2C] scan done, "));
  Serial.print(found);
  Serial.println(F(" device(s)."));
}

void initSensor() {
  // Keep retrying so you get live feedback while fixing the wiring.
  while (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {  // 400 kHz
    Serial.println(F("[ERR] MAX30102 not found at 0x57. Re-checking in 2 s..."));
    i2cScan();
    delay(2000);
  }
  Serial.println(F("[OK ] MAX30102 found."));

  // Sensible defaults for finger heart-rate sensing.
  //   ledBrightness 0x1F, sampleAvg 8, mode 2 (Red+IR),
  //   sampleRate 100 Hz, pulseWidth 411us, adcRange 4096
  particleSensor.setup(0x1F, 8, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x0A);   // dim red — IR is used for beats
  particleSensor.setPulseAmplitudeIR(0x1F);    // a bit more IR power
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n=== MAX30102 on XIAO ESP32-C6 ==="));

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  i2cScan();
  initSensor();

  Serial.println(F("\nReady. Rest a fingertip gently on the sensor window.\n"));
}

void loop() {
  long irValue = particleSensor.getIR();

  // Beat detection on the IR channel.
  if (checkForBeat(irValue)) {
    long now = millis();
    long delta = now - lastBeat;
    lastBeat = now;

    float bpm = 60.0 / (delta / 1000.0);
    if (bpm > 20 && bpm < 255) {
      rates[rateSpot++] = (byte)bpm;
      rateSpot %= RATE_SIZE;

      int sum = 0;
      for (byte i = 0; i < RATE_SIZE; i++) sum += rates[i];
      beatAvg = sum / RATE_SIZE;
      beatsPerMinute = bpm;
    }
  }

  // Telemetry — Arduino Serial Plotter friendly.
  Serial.print(F("IR="));
  Serial.print(irValue);
  Serial.print(F("\tBPM="));
  Serial.print(beatsPerMinute, 1);
  Serial.print(F("\tAvgBPM="));
  Serial.print(beatAvg);

  if (irValue < FINGER_THRESHOLD)
    Serial.print(F("\t(no finger)"));

  Serial.println();
  delay(20);   // ~50 Hz print rate
}
