/*
 * MAX30102 -> BLE 原始数据流  |  Seeed XIAO ESP32-C6
 * ==================================================================
 * 架构: 固件只做"读传感器 FIFO + 打包 + BLE 发原始 IR/RED 样本"。
 *       所有算法(去直流/心跳/RR/HRV/血氧)都在浏览器 JS 里做。
 *       —— C6 无硬件 FPU, 不适合跑重算法; 把计算放电脑端最稳、最好迭代。
 *
 * 数据通路(抗丢样): 传感器读取与 BLE 发送解耦为两条执行流——
 *   [sensorTask] 高优先级 FreeRTOS 任务: 每 5ms 排空 MAX30102 FIFO -> 写入样本队列。
 *                绝不被 BLE 发送阻塞(SparkFun 库内部仅缓存 4 样本, 100Hz 下 loop 停顿
 *                >40ms 即静默丢样, 故必须独立及时排空)。并周期检测 OVF/指针健康。
 *   [loop]       消费样本队列 -> 按"连续序号"组批 -> 按协商 MTU 分包 BLE notify。
 *   绝对样本序号 sampleIndex 反映"真实流逝的样本数"(含掉线/溢出补偿), 浏览器据此
 *   重建时间轴; 任何序号断点都会让浏览器重置连续性, 避免把丢失时段当成连续时间
 *   (这对 HRV/RR 间期尤其关键)。
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const int PIN_SDA = 22;  // D4
static const int PIN_SCL = 23;  // D5

#define SVC_UUID  "12345678-1234-5678-1234-56789abcdef0"
#define CHR_UUID  "12345678-1234-5678-1234-56789abcdef1"
#define DEV_NAME  "XIAO-HR"
#define FS_HZ     100           // 有效采样率(网页需与此一致)

#define SENSOR_ADDR     0x57
#define REG_OVF_COUNTER 0x05    // MAX30102 OVF_COUNTER 寄存器(库未暴露, 直接读)

MAX30105 sensor;
volatile bool sensorOk = false;

// ---- 样本队列: sensorTask(生产) -> loop(消费), 解耦读取与发送 ----
struct Smp { uint32_t idx, ir, red; };
QueueHandle_t sampleQ = nullptr;
const int QLEN = 256;                 // ~2.5s 缓冲 @100Hz, 足以吸收 BLE 抖动

volatile uint32_t sampleIndex = 0;    // 绝对样本序号(仅 sensorTask 写; 含丢样/掉线补偿)
volatile uint32_t lostTotal   = 0;    // 累计丢样估计(串口可观测)
volatile long     lastIR      = 0;
volatile uint32_t samplesThisSec = 0;

// ---- 打包(loop 私有: 仅消费线程访问)----
const int BATCH = 8;                  // 每包样本数 -> 约 12.5 包/秒(80ms 缓冲, 兼顾低延迟与 BLE 压力)
uint32_t batchIR[BATCH], batchRED[BATCH];
int      batchN = 0;
uint32_t batchFirst = 0;              // 本批第一个样本的绝对序号
uint32_t batchExpect = 0;            // 下一个期望的连续序号(用于检测序号断点)
unsigned long lastSendMs = 0;

// ---- 串口台架统计 ----
unsigned long lastStat = 0;

// ---- BLE ----
BLEServer* pServer = nullptr;
BLECharacteristic* pChar = nullptr;
volatile bool deviceConnected = false, oldDeviceConnected = false;
class ServerCB : public BLEServerCallbacks {
  // 带 param 的重载会随连接事件一并被调用, 可拿到对端地址
  void onConnect(BLEServer* s, esp_ble_gatts_cb_param_t* param) override {
    deviceConnected = true;
    // 主动请求较快且稳定的连接间隔(15~30ms, 0 从机延迟, 4s 超时), 降低数据延迟
    s->updateConnParams(param->connect.remote_bda, 0x0C, 0x18, 0, 400);
  }
  void onDisconnect(BLEServer*) override { deviceConnected = false; }
};

bool initSensor() {
  if (!sensor.begin(Wire, I2C_SPEED_FAST)) return false;   // 400 kHz
  sensor.setup(0x1F, 4, 2, 400, 411, 4096);   // 亮度, 平均4, 模式2, 400 -> 100Hz, 411us, 4096
  sensor.setPulseAmplitudeRed(0x24);
  sensor.setPulseAmplitudeIR(0x24);
  sensor.clearFIFO();                          // 清空芯片 FIFO, 从干净状态开始
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
  adv->setMinPreferred(0x06); adv->setMaxPreferred(0x12);   // 7.5~22.5ms 偏好(原代码两次都是 setMin, 漏了 setMax)
  BLEDevice::startAdvertising();
}

// ============================ 传感器任务 ============================
// 高优先级独立任务: 只负责"尽快排空 FIFO 入队列", 不碰 BLE。
// 每 5ms 醒来一次(<< 库 4 样本缓冲对应的 40ms 上限), 即使 BLE 发送在 loop 里阻塞,
// 本任务凭更高优先级抢占, 保证 FIFO 不溢出、不丢样。
void sensorTask(void*) {
  unsigned long lastSampleMs = millis(), lastChk = 0, lastRetry = 0;
  int glitch = 0;
  for (;;) {
    if (!sensorOk) {
      if (millis() - lastRetry > 2000) {
        lastRetry = millis();
        sensorOk = initSensor();
        if (sensorOk) {
          // 掉线恢复: 把缺失时段折算成样本数推进绝对序号 -> 制造序号断点,
          // 浏览器据此重置连续性, 且时间轴保持与真实流逝一致(不把掉线段当连续)。
          uint32_t gap = (uint32_t)((uint64_t)(millis() - lastSampleMs) * FS_HZ / 1000);
          sampleIndex += gap; lostTotal += gap;
          lastSampleMs = millis();
        }
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    sensor.check();
    while (sensor.available()) {
      uint32_t ir  = sensor.getFIFOIR();
      uint32_t red = sensor.getFIFORed();
      sensor.nextSample();
      lastIR = ir; lastSampleMs = millis(); samplesThisSec++;
      Smp s{ sampleIndex, ir, red };
      // 队列满(消费者饿死)则丢值, 但序号仍前进 -> 浏览器看到断点而非"被压缩的时间"
      if (xQueueSend(sampleQ, &s, 0) != pdTRUE) lostTotal++;
      sampleIndex++;
    }

    // 周期性健康检查(~200ms): OVF 溢出补偿 + I2C 指针合理性
    if (millis() - lastChk > 200) {
      lastChk = millis();
      // 芯片硬 FIFO 溢出丢样: OVF_COUNTER 0..0x1F, 取 1..0x1E 视为可信丢样数(0x1F 多为饱和/总线异常 0xFF&0x1F, 忽略)
      uint8_t ovf = sensor.readRegister8(SENSOR_ADDR, REG_OVF_COUNTER);
      if (ovf > 0 && ovf < 0x1F) { sampleIndex += ovf; lostTotal += ovf; }
      // I2C 受扰时指针会读回越界值(如 0xFF); 连续异常则重 init(配合 Wire.setTimeOut 防总线卡死)
      uint8_t wr = sensor.getWritePointer(), rd = sensor.getReadPointer();
      if (wr > 31 || rd > 31) { if (++glitch >= 3) { sensorOk = false; glitch = 0; } }
      else glitch = 0;
    }

    // 看门狗: 5s 无样本 = 真实掉线(线松/掉电), 触发重 init(阈值 5s 避免 BLE 短暂争用误判)
    if (millis() - lastSampleMs > 5000) sensorOk = false;

    vTaskDelay(pdMS_TO_TICKS(5));   // 每 5ms 排空一次
  }
}

// ============================ BLE 发送(消费线程)============================
// 发送 [base, base+n) 这段连续样本为一个 notify 包(二进制小端)
void notifyChunk(uint32_t firstAbs, int base, int n) {
  static uint8_t pkt[5 + BATCH * 8];
  pkt[0] = firstAbs & 0xFF; pkt[1] = (firstAbs >> 8) & 0xFF;
  pkt[2] = (firstAbs >> 16) & 0xFF; pkt[3] = (firstAbs >> 24) & 0xFF;
  pkt[4] = (uint8_t)n;
  int off = 5;
  for (int i = 0; i < n; i++) {
    uint32_t ir = batchIR[base + i], rd = batchRED[base + i];
    pkt[off++] = ir & 0xFF; pkt[off++] = (ir >> 8) & 0xFF; pkt[off++] = (ir >> 16) & 0xFF; pkt[off++] = (ir >> 24) & 0xFF;
    pkt[off++] = rd & 0xFF; pkt[off++] = (rd >> 8) & 0xFF; pkt[off++] = (rd >> 16) & 0xFF; pkt[off++] = (rd >> 24) & 0xFF;
  }
  pChar->setValue(pkt, off);
  pChar->notify();
}

// 把当前批(保证内部序号连续)发出; 按协商 MTU 分包, 防止大包在 MTU 未协商成功时被丢弃/截断
void flushBatch() {
  if (batchN == 0) return;
  if (deviceConnected) {
    // 单 notify 有效负载 = MTU-3; 包头 5 字节; 每样本 8 字节
    uint16_t mtu = pServer->getPeerMTU(pServer->getConnId());
    if (mtu < 23) mtu = 23;
    int maxS = ((int)mtu - 3 - 5) / 8;
    if (maxS < 1)     maxS = 1;
    if (maxS > BATCH) maxS = BATCH;
    int sent = 0;
    while (sent < batchN) {
      int n = batchN - sent; if (n > maxS) n = maxS;
      notifyChunk(batchFirst + sent, sent, n);   // 批内连续 -> firstAbs = batchFirst+offset 成立
      sent += n;
    }
  }
  batchN = 0;
  lastSendMs = millis();
}

// 排空样本队列 -> 组批(遇序号断点先 flush, 让浏览器重置连续性) -> 发送
void consume() {
  Smp s;
  while (xQueueReceive(sampleQ, &s, 0) == pdTRUE) {
    if (batchN > 0 && s.idx != batchExpect) flushBatch();      // 序号断点(丢样/掉线/溢出)-> 切包
    if (batchN == 0) { batchFirst = s.idx; batchExpect = s.idx; }
    batchIR[batchN] = s.ir; batchRED[batchN] = s.red; batchN++; batchExpect++;
    if (batchN >= BATCH) flushBatch();
  }
  // 尾部低延迟: 即使未满一批, 超过 ~60ms 也发出, 避免静止期数据滞留
  if (batchN > 0 && millis() - lastSendMs > 60) flushBatch();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n=== MAX30102 BLE 原始数据流 (XIAO ESP32-C6) ==="));
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(50);                          // I2C 操作 50ms 超时, 防总线受扰卡死整个执行流
  sampleQ = xQueueCreate(QLEN, sizeof(Smp));
  sensorOk = initSensor();
  Serial.println(sensorOk ? F("[OK ] sensor ready") : F("[WARN] sensor not found"));
  initBLE();
  Serial.print(F("[BLE] advertising as ")); Serial.println(DEV_NAME);
  Serial.print(F("[INFO] FS=")); Serial.print(FS_HZ); Serial.println(F("Hz, 算法全部在浏览器侧"));
  // 传感器读取独立成高优先级任务(优先级 2 > Arduino loop 的 1), 与 BLE 发送解耦
  xTaskCreate(sensorTask, "sensorTask", 8192, nullptr, 2, nullptr);
}

void loop() {
  if (!deviceConnected && oldDeviceConnected) { delay(300); pServer->startAdvertising(); }
  oldDeviceConnected = deviceConnected;
  if (!deviceConnected) batchN = 0;             // 未连接: 丢弃待发批(队列仍由 consume 排空, 不溢出)

  consume();                                    // 排空队列 -> 批 -> MTU 分包 notify

  if (millis() - lastStat > 1000) {
    lastStat = millis();
    Serial.printf("sps=%lu idx=%lu IR=%ld lost=%lu conn=%d\n",
                  (unsigned long)samplesThisSec, (unsigned long)sampleIndex,
                  (long)lastIR, (unsigned long)lostTotal, deviceConnected ? 1 : 0);
    samplesThisSec = 0;
  }

  delay(2);                                     // 让出 CPU(consume 已尽量及时; sensorTask 更高优先级会抢占)
}
