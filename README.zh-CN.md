# XIAO ESP32-C6 + MAX30102：用 BLE 传脉搏、血氧和 HRV

> [English](README.md) | 中文 | [日本語](README.ja.md)

一个小小的心率、血氧、HRV 监测器。板子读光电脉搏传感器，把原始光信号通过蓝牙发出去，
剩下的全交给网页。

做它的起因很简单：我想看看自己的心率变异性，又不想专门买个胸带；顺便也好奇，一个几块
钱的传感器加一个浏览器，到底能做到什么程度。

## 思路

设备这一端是故意做“笨”的。ESP32 只管读 MAX30102，把原始 IR/RED 样本按 100Hz 通过 BLE
发出来。不滤波、不检测心跳、芯片上不做任何计算。

把这些数字变成有意义的东西，全部放在浏览器里用 JavaScript 做：心跳检测、拍与拍之间的间
隔、HRV、血氧、实时波形。

这么拆有两个原因：

- C6 没有硬件浮点单元。比整数运算重一点的东西都得靠软件模拟，很慢；再加上 BLE 协议栈也
  在抢这颗单核，偶尔就足以把主循环拖住。（这事坑了我一个晚上。后来固件改成尽量什么都不
  做，问题就没了。）
- 算法放在网页里，改一处就是改段文字。每次想把心跳检测的参数挪一点点就要重新烧录，我实
  在受够了。现在存一下 HTML、刷新就行。

整套东西在 Windows、macOS、Linux 上表现一致，只要有 Chrome 或 Edge，什么都不用装。

这是个练手项目，不是医疗设备。MAX30102 测的是脉搏率变异（PRV）。静息时它和心电得到的
HRV 还算接近，一动就不行，绝对数值别太当真。

## 硬件

- Seeed XIAO ESP32-C6
- MAX30102 模块（常见的紫色 GY-MAX30102 就行）
- 四根杜邦线

接线（I²C）：

| MAX30102 | XIAO ESP32-C6 |
|----------|---------------|
| VIN      | 3V3           |
| GND      | GND           |
| SDA      | D4 (GPIO22)   |
| SCL      | D5 (GPIO23)   |

用 3V3 供电，别接 5V。传感器在 I²C 地址 0x57。如果扫描扫不到，先查 SDA 和 SCL 是不是接
反了，再查别的。我当初就是接反了，而且症状很气人：上拉电平正常、总线看着也健康，芯片就
是不应答。

## 固件

用 arduino-cli 和乐鑫的 ESP32 core 构建。

```bash
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "SparkFun MAX3010x Pulse and Proximity Sensor Library"

# 编译并上传，端口换成你自己的
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6 -u \
  -p /dev/cu.usbmodemXXXX max30102_ble
```

MAX30102 配成 Red+IR 模式、内部 400Hz 采样、4 倍平均，于是每秒输出 100 个有效样本。固件
把传感器 FIFO 里的数据取出来、打成小包、发出去。它不存历史、不做任何 DSP——你去读那段主
循环，几乎是无聊的，而这正是目的。

广播名是 `XIAO-HR`。USB 串口 115200，每秒打印一行（采样率、累计样本号、最近的 IR 值、有
没有连上中心设备），不用放手指就能确认它在跑。

### BLE 数据格式

一个 service，一个 notify 特征。每条通知是小端二进制包：

```
[uint32 firstIndex][uint8 count][ count × (uint32 ir, uint32 red) ]
```

- `firstIndex` 是这一包里第一个样本的绝对序号。网页用它重建时间轴，也用它发现丢包。
- `count` 当前是 16，所以大约每秒六条通知。包大一点、通知就少一点、对射频的压力也小一
  点；同时又小到能塞进一般协商出来的 MTU。
- 采样率固定 100Hz，网页里写死这个值，把样本序号换算成时间。

Service `12345678-1234-5678-1234-56789abcdef0`，特征 `…def1`。

## 网页

`web/index.html` 是单个文件：没有依赖、不用构建、不用服务器，双击就能开。用 Chrome 或
Edge——Safari 和 Firefox 不支持 Web Bluetooth。

连接、选 `XIAO-HR`、指尖盖住光窗别动。几秒后就有脉搏波形（每个检测到的心拍上有个点）、
心率、血氧，还有 HRV 面板。界面有中、英、日三种语言，会记录每一个数据包、能导出 CSV，断
线还会自己重连。

底下发生了什么：

- **心跳检测**：原始 IR 先减去一个慢速指数滑动平均（时间常数大约两秒）做高通，去掉基线
  漂移又不会把脉搏一起抹平——这个常数取错了，就会变成什么都检测不到。再用一个轻低通把剩
  下的信号磨平。然后用一个从衰减包络算出来的自适应阈值找峰，并设了不应期，免得把重搏切
  迹当成第二次心跳。
- **RR 计时**：每个峰用它前后三点做抛物线拟合细化，把间隔分辨率压到 10ms 采样间距以下。
  RR 是按样本序号算的，不是按墙上时钟，所以蓝牙抖动不会把它搅花。
- **HRV**：只做时域——平均 RR、RMSSD、pNN50、SDNN，外加 Poincaré 散点的 SD1/SD2。在这之
  前会先把偏离局部中位数超过 30% 的间隔剔掉，免得一次漏拍或多拍就把数值毁了。
- **血氧**：常规的比值之比，R = (AC_red/DC_red) / (AC_ir/DC_ir)，取几秒的窗口，用标准线
  性近似映射再钳位。没有标定，看个大概就好。

心跳检测的参数都在脚本顶部一个对象里（`const P`）。因为东西都在网页里，调参就是改一下、
刷新一下。

## 目录

```
max30102_ble/    固件——通过 BLE 流原始 IR/RED
web/index.html   网页
i2c_diag/        我接线时用的独立 I²C 扫描器
max30102_xiao/   最早的版本，只走串口，留作参考
```

## 许可

MIT。
