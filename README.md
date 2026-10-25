# M1 传感器主控固件

斐讯 M1 空气检测仪 ATSAMD20G18A 主控的开源固件，负责传感器采集、屏幕、时钟、按键、蜂鸣器和主控升级。Wi-Fi、MQTT 和 WebUI 由配套[联网模块固件](https://github.com/phicomm-io/phicomm-community-firmware)提供，两个处理器分别升级。

## 兼容性

优先建议原厂主控 **11.x** 用户使用。原厂 **17.1.2.3 / 17.x** 差异较大，取得对应镜像并完成分析前暂不推荐使用。

WebUI 会分别显示联网模块版本和传感器 MCU 原始版本。原厂版本与开源固件版本独立编号，不按大小判断兼容性；无法读取时显示未知。

## 功能

- 甲醛、PM1.0 / PM2.5 / PM10、颗粒计数、温湿度采集及完整帧诊断。
- PM2.5 尖峰抑制和自适应平滑，持续升高时快速跟随；原始帧仍可查询。
- 日期、时间、亮度、污染物灯色、自定义告警阈值及 CO₂ 扩展显示。
- 甲醛与 PM2.5 超阈值蜂鸣报警分别开关，默认关闭。
- 按键、提示音、RTTTL 音乐、闹钟与蜂鸣器告警。
- 主控版本查询、串口升级、独立 Boot 和升级中断恢复。

v1.0.0 整合现有功能。常用设置通过配套 WebUI 操作；`0x70` 扩展帧保留原有操作码，诊断和硬件覆盖命令单独列在[协议说明](docs/debug-protocol.md)中。

## 编译

需要 C 编译器、Make、Python 3 和 GNU Arm Embedded GCC / Binutils：

```sh
make test sanitize
make ota
# 指定工具链：make ota CROSS_COMPILE=/path/to/arm-none-eabi-
```

OTA 包位于 `build/ota/m1-mcu-ota.bin`，同目录提供清单和 SHA-256。`build/m1-app.bin` 链接地址为 `0x4000`，`build/m1-boot.bin` 链接地址为 `0`，两个镜像不能互换。

ARM 时钟模型检查：

```sh
python3 -m venv build/hal-venv
build/hal-venv/bin/pip install -r requirements-hal.txt
make hal-check HAL_PYTHON=build/hal-venv/bin/python
```

## 使用与升级

- [安装、OTA 与恢复](docs/ota.md)
- [通信协议](docs/protocol.md) · [扩展命令](docs/debug-protocol.md)
- [告警阈值](docs/thresholds.md) · [音乐](docs/music.md) · [传感器帧诊断](docs/sensor-frames.md)
- [硬件接线](docs/hardware.md) · [实现说明](docs/implementation.md)
- [开发与兼容性规范](CONTRIBUTING.md) · [版本规则](docs/versioning.md)

应用沿用原厂暂存区 `0x22000`，最大镜像 65535 字节。独立 Boot 使用 CRC32 日志处理搬运中断；CRC 不提供签名验证或旧版本回滚。保留原厂 Boot 时沿用原厂恢复能力；无有效应用时需要 SWD 恢复。升级步骤见 OTA 文档。

## License

本项目采用 [GPL-3.0](LICENSE)。[third_party](third_party) 中的 Microchip SAMD20 设备头文件和 ARM CMSIS 保留各自许可。

[English](README.en.md)
