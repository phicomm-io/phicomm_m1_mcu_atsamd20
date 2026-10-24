# phicomm_m1_mcu_atsamd20

斐讯 M1 空气检测仪 **ATSAMD20G18A 主控**的原厂逻辑分析与开源固件。联网模块仍负责 Wi-Fi 和云端通信；本仓库实现传感器、显示、RTC、按键、蜂鸣器及主控升级。

**已具备完整应用与独立 Boot 的代码、主机测试和 ARM 构建。尚未刷入实物验证，生成的是开发候选镜像，不是已验收的替代固件。** 原厂备份未公开，也没有执行设备写入或发布刷机版本。

## 实现范围

| 模块 | 实现 |
| --- | --- |
| 系统 | 启动与 41 项向量、8 MHz 时钟、1 ms SysTick、协作调度、中断、约 8 秒 WDT |
| 联网通信 | 三路 UART，IRQ 接收/发送队列、帧流恢复、超时、启动/时间/按键/错误通知 |
| 甲醛 | 初始化探测、周期请求、校验、重试、原始值查询、原厂低值显示处理 |
| PM2.5 | 32 字节双模式、两帧/四帧预热、突变过滤、探测应答与流恢复 |
| 温湿度 | I²C/CRC、f32d 状态、限流软复位、原厂定点补偿及显示开关输入 |
| 显示 | 完整 29 字节 RAM、数字/日期/时间/负号/单位、亮度/开关、全段测试、独立污染物灯色 |
| RTC | DS1302 兼容三线读写、BCD、日历走时、硬件周期同步、读失败时 RAM 续时 |
| 按键 | PA10/EIC、20 ms 消抖、短按切屏、长按事件和网络重配通知 |
| 蜂鸣器 | TC1 PWM、持续/200 ms 单次/重复鸣叫、污染物告警联动 |
| 状态 | 有效采集更新健康标志、三次坏帧/6 秒超时失效、错误通知、恢复重试 |
| PA24 | 启动释放 CHIP_EN、原厂等待结构、OTA 提交后 4000/1000 ms 联动复位 |
| 调试 | `0x70` PWM、数值/段 RAM/全段覆盖、状态、PM 模式选择、租期恢复 |
| MCU OTA | `0x12–0x17` 链路、暂存/回读/CRC、元数据提交、独立 Boot 搬运及中断恢复 |

颜色由用户实机确认：**组 2 / 0x13 绿，组 3 / 0x12 黄，组 1 / 0x0c 红**。甲醛掩码 `0x70` 与 PM 掩码 `0x85` 独立，分别按 `100/300` 和 `75/150` 选择颜色。

原厂程序使用 RTOS；新固件采用协作调度。采样周期、消抖与长按秒数、失效策略及蜂鸣器节奏是明确实现的策略，尚未逐项核准原厂实际时间。详见 [实现与差异](docs/implementation.md)、[协议](docs/protocol.md)、[硬件](docs/hardware.md)。

## 编译与验证

主机测试需要 C 编译器、Make 和 Python 3：

```sh
make test sanitize
```

ARM 构建需要 GNU Arm Embedded GCC / Binutils：

```sh
# Debian / Ubuntu
sudo apt install --no-install-recommends gcc-arm-none-eabi binutils-arm-none-eabi
make ota
# 非默认工具链可用：make ota CROSS_COMPILE=/path/to/arm-none-eabi-
```

OTA 包：`build/ota/m1-mcu-ota.bin`、`.json`、`SHA256SUMS`。其他产物：`build/m1-app.elf`、`build/m1-app.bin`（链接地址 `0x4000`），`build/m1-boot.elf`、`build/m1-boot.bin`（链接地址 `0`）。构建会检查可加载段、向量表和 RAM 边界；每个镜像附带 SHA-256/工具链/源码状态记录。两者是独立产物，不能互换地址。没有自动刷机步骤。

[验证记录](docs/validation.md) 分开记录软件、指令对照和实物证据。公开 CI 运行主机测试、ASan/UBSan、应用/Boot 构建和实际 OTA 镜像的接收/搬运模型验证，上传 OTA 候选及独立调试/恢复产物。下载与使用见 [OTA 文档](docs/ota.md)，Wi-Fi MCU 控制 PWM/屏幕见 [调试协议](docs/debug-protocol.md)。不自动刷机或发布正式 Release。

## 升级与恢复边界

应用沿用原厂暂存区 `0x22000` 和基础元数据 `0x3ff00`，受 16 位长度约束，最大镜像 65535 字节。独立 Boot 额外检查 `0x3ff40` 的 CRC32 日志，在搬运中断后重新校验并从暂存区重做；已完成的应用由全镜像 CRC 识别，保留提交日志至下一次升级。CRC **不是签名验证**，也没有旧版本回滚副本。

保留原厂 Boot 时，它只使用原有头部，忽略扩展日志，**不能获得新 Boot 的 CRC 和恢复保证**。新 Boot 拒绝没有有效扩展日志的旧式 pending 升级。无有效应用、损坏镜像或无效日志时停留恢复状态，需要 SWD 处理；它没有另一个独立串口救援下载器。细节见 [实现说明](docs/implementation.md)。

## 用自己的备份复现

```sh
python3 tools/analyze_stock.py /path/to/your/m1-mcu-backup.bin
python3 -m venv build/analysis-venv
build/analysis-venv/bin/pip install -r requirements-analysis.txt
build/analysis-venv/bin/python tools/verify_stock.py /path/to/your/m1-mcu-backup.bin
build/analysis-venv/bin/python tools/verify_runtime_stock.py /path/to/your/m1-mcu-backup.bin
build/analysis-venv/bin/python tools/verify_peripherals_stock.py /path/to/your/m1-mcu-backup.bin
```

固定入口的对照工具只接受 [已分析哈希](docs/stock-image.json)。原厂函数只在 Unicorn 的模拟 Flash/SRAM 中执行，不访问硬件。分析依据见 [stock-analysis.md](docs/stock-analysis.md)。

## 来源与许可

原创实现与文档采用 [MIT](LICENSE)。允许复用的库已纳入：Microchip SAMD20 DFP 3.7.231 设备头文件、ARM CMSIS 5.9.0 Cortex-M0+ / GCC 头文件，均保留 Apache-2.0 许可证、固定版本和来源记录，见 [third_party](third_party)。没有包含原厂镜像或反编译源码。详见 [PROVENANCE.md](PROVENANCE.md)。

联网模块固件位于 [phicomm-community-firmware](https://github.com/phicomm-io/phicomm-community-firmware)，与本仓库的主控镜像分别升级。
