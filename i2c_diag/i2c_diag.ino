/*
 * I2C bus electrical diagnostic — XIAO ESP32-C6 + MAX30102
 * Reads the idle logic level of SDA (D4/GPIO22) and SCL (D5/GPIO23)
 * to tell whether the sensor is powered and the bus has pull-ups.
 *
 *  floating  pull-up   meaning
 *  --------  -------    --------------------------------------------------
 *  1   1     1   1      External pull-ups present -> sensor powered & wired.
 *                       If scan still finds nothing: SDA/SCL swapped, wrong
 *                       device, or dead sensor.
 *  0   0     1   1      No external pull-ups seen -> sensor NOT powered
 *                       (VIN not on 3V3) or module has none. Check power.
 *  x   x     0   x      A line stays LOW even with pull-up -> short to GND
 *  (or)x x   x   0      or that wire (SDA/SCL) miswired/shorted.
 */
#include <Arduino.h>
#include <Wire.h>

const int PIN_SDA = 22;  // D4
const int PIN_SCL = 23;  // D5

void lineReport() {
  // 1) Read with NO pull-up (true idle level set by external circuitry)
  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);
  delay(5);
  int sdaF = digitalRead(PIN_SDA);
  int sclF = digitalRead(PIN_SCL);

  // 2) Read with the chip's internal pull-up engaged
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, INPUT_PULLUP);
  delay(5);
  int sdaP = digitalRead(PIN_SDA);
  int sclP = digitalRead(PIN_SCL);

  Serial.printf("[LINES] floating: SDA=%d SCL=%d | internal-pullup: SDA=%d SCL=%d -> ",
                sdaF, sclF, sdaP, sclP);

  if (sdaF == 1 && sclF == 1)
    Serial.println("external pull-ups OK (powered+wired). If no device: check swap/address/sensor.");
  else if (sdaP == 1 && sclP == 1)
    Serial.println("NO external pull-ups -> sensor likely UNPOWERED (VIN!=3V3) or no pull-ups.");
  else
    Serial.println("a line stuck LOW -> short to GND or SDA/SCL miswire.");
}

void i2cScan() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);  // gentle 100 kHz
  byte found = 0;
  for (byte a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] device @ 0x%02X%s\n", a, (a == 0x57) ? "  <- MAX30102" : "");
      found++;
    }
  }
  Serial.printf("[I2C] %d device(s) @100kHz\n", found);
  Wire.end();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== I2C electrical diagnostic (XIAO ESP32-C6) ===");
}

void loop() {
  lineReport();
  i2cScan();
  Serial.println("---");
  delay(1500);
}
