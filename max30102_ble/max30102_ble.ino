/*
 * MAX30102 -> BLE 原始数据流  |  Seeed XIAO ESP32-C6
 * ==================================================================
 * 架构: 固件只做"读传感器 FIFO + 打包 + BLE 发原始 IR/RED 样本"。
 *       所有算法(去直流/心跳/RR/HRV/血氧)都在浏览器 JS 里做。
 *       —— C6 无硬件 FPU, 不适合跑重算法; 把计算放电脑端最稳、最好迭代。
 *
 * I2C: VIN->3V3, GND->GND, SDA->D4(GPIO22), SCL->D5(GPIO23), 地址 0x57
 * 采样: ledMode2(红外+红), 有效 100Hz (sampleRate400 / avg4)
 *
 * BLE: 名 XIAO-HR, Service ...def0, Char(notify) ...def1
 *   通知负载为二进制小端包, 每包 N 个样本:
 *     [0..3]  uint32  firstIndex  本包第一个样本的绝对序号(用于排序/丢包检测)
 *     [4]     uint8   count       本包样本数 N
 *     [5..]   N × { uint32 ir ; uint32 red }   (各小端)
 *   采样率固定 FS=100Hz, 浏览器据此把样本序号换算成时间。
 *   传感器不在线时不发数据(浏览器据"无数据"判离线)。
 * ==================================================================
 */

#include <Wire.h>
#include "MAX30105.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

static const int PIN_SDA = 22;  // D4
static const int PIN_SCL = 23;  // D5

#define SVC_UUID  "12345678-1234-5678-1234-56789abcdef0"
#define CHR_UUID  "12345678-1234-5678-1234-56789abcdef1"
#define DEV_NAME  "XIAO-HR"
#define FS_HZ     100           // 有效采样率(网页需与此一致)

MAX30105 sensor;
bool sensorOk = false;
unsigned long lastSensorRetry = 0, lastSampleMs = 0;

// ---- 批量打包 ----
const int BATCH = 16;                 // 每包样本数 -> 约 6 包/秒(更少通知=更小 BLE 压力)
uint32_t batchIR[BATCH], batchRED[BATCH];
int      batchN = 0;
uint32_t sampleIndex = 0;             // 绝对样本序号
long lastIR = 0;

// ---- 串口台架统计 ----
unsigned long lastStat = 0;
uint32_t samplesThisSec = 0;

// ---- BLE ----
BLEServer* pServer = nullptr;
BLECharacteristic* pChar = nullptr;
volatile bool deviceConnected = false, oldDeviceConnected = false;
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { deviceConnected = true; }
  void onDisconnect(BLEServer*) override { deviceConnected = false; }
};

bool initSensor() {
  if (!sensor.begin(Wire, I2C_SPEED_FAST)) return false;   // 400 kHz
  sensor.setup(0x1F, 4, 2, 400, 411, 4096);   // 亮度, 平均4, 模式2, 400 -> 100Hz, 411us, 4096
  sensor.setPulseAmplitudeRed(0x24);
  sensor.setPulseAmplitudeIR(0x24);
  return true;
}

void initBLE() {
  BLEDevice::init(DEV_NAME);
  BLEDevice::setMTU(247);                       // 请求较大 MTU 以便一次发整批
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());
  BLEService* svc = pServer->createService(SVC_UUID);
  pChar = svc->createCharacteristic(
      CHR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pChar->addDescriptor(new BLE2902());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06); adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
}

void sendBatch() {
  // 组二进制小端包
  static uint8_t pkt[5 + BATCH * 8];
  uint32_t first = sampleIndex - batchN;
  pkt[0] = first & 0xFF; pkt[1] = (first >> 8) & 0xFF;
  pkt[2] = (first >> 16) & 0xFF; pkt[3] = (first >> 24) & 0xFF;
  pkt[4] = (uint8_t)batchN;
  int off = 5;
  for (int i = 0; i < batchN; i++) {
    uint32_t ir = batchIR[i], rd = batchRED[i];
    pkt[off++] = ir & 0xFF; pkt[off++] = (ir >> 8) & 0xFF; pkt[off++] = (ir >> 16) & 0xFF; pkt[off++] = (ir >> 24) & 0xFF;
    pkt[off++] = rd & 0xFF; pkt[off++] = (rd >> 8) & 0xFF; pkt[off++] = (rd >> 16) & 0xFF; pkt[off++] = (rd >> 24) & 0xFF;
  }
  pChar->setValue(pkt, off);
  if (deviceConnected) pChar->notify();
  batchN = 0;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n=== MAX30102 BLE 原始数据流 (XIAO ESP32-C6) ==="));
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  sensorOk = initSensor();
  Serial.println(sensorOk ? F("[OK ] sensor ready") : F("[WARN] sensor not found"));
  initBLE();
  Serial.print(F("[BLE] advertising as ")); Serial.println(DEV_NAME);
  Serial.print(F("[INFO] FS=")); Serial.print(FS_HZ); Serial.println(F("Hz, 算法全部在浏览器侧"));
}

void loop() {
  if (!deviceConnected && oldDeviceConnected) { delay(300); pServer->startAdvertising(); }
  oldDeviceConnected = deviceConnected;

  if (!sensorOk) {
    if (millis() - lastSensorRetry > 2000) { lastSensorRetry = millis(); sensorOk = initSensor(); if (sensorOk) lastSampleMs = millis(); }
    return;
  }

  sensor.check();
  while (sensor.available()) {
    uint32_t ir  = sensor.getFIFOIR();
    uint32_t red = sensor.getFIFORed();
    sensor.nextSample();
    lastIR = ir; lastSampleMs = millis();
    samplesThisSec++;

    batchIR[batchN] = ir; batchRED[batchN] = red; batchN++;
    sampleIndex++;
    if (batchN >= BATCH) sendBatch();
  }

  // 看门狗: 仅用于"真实掉线"(如线松), 阈值放到 5s 避免 BLE 争用造成的短暂停顿误触发。
  // 新固件恢复分支只是重试 init、不发误导性零帧, 也不重置任何检测器(检测在浏览器侧)。
  if (millis() - lastSampleMs > 5000) { sensorOk = false; batchN = 0; return; }

  // 串口台架: 每秒打印采样率与最近值(无需手指即可确认数据流稳定)
  if (millis() - lastStat > 1000) {
    lastStat = millis();
    Serial.printf("sps=%lu idx=%lu IR=%ld conn=%d\n",
                  (unsigned long)samplesThisSec, (unsigned long)sampleIndex, lastIR, deviceConnected ? 1 : 0);
    samplesThisSec = 0;
  }
}
