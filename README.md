# phicomm_m1_mcu_atsamd20

斐讯 M1 空气检测仪 **ATSAMD20 主控**的原厂固件分析与开源固件工程。目标是让传感器、显示、按键和蜂鸣器能够由可维护的开源代码继续工作，并兼容现有 EMW3080 联网模块。

**当前是分析和协议核心阶段，还不能替代原厂主控固件。** 已实现可运行的主机协议核心、传感器报文校验、灯组选择及 RAM 掩码；实际 UART/I²C/定时器/屏幕数字驱动尚未接入。ARM 产物 `core-link-check.elf` 只检查编译、启动代码和内存布局，不提供刷机包。

## 已确认的原厂逻辑

- 本次实物读取识别为 **ATSAMD20G18A，256 KiB Flash / 32 KiB SRAM**；不能沿用此前 G17A 的容量假设。
- Bootloader 从 `0x0000` 启动，应用向量表在 `0x4000`，应用版本为 `11.1.2.1`。
- 应用有 10 个任务，负责显示、甲醛、PM2.5、温湿度、RTC、串口、蜂鸣器与按键等。
- MCU 与联网模块之间，普通命令 12 字节、返回 20 字节，使用 `23 … 21` 帧边界。
- 采样返回的 `f[11]/f[12]/f[13]` 分别来自 PM2.5、甲醛、温湿度状态变量；负温度另有符号位。
- PA10 按键短按切换显示并发送 `0x0f`；长按路径发送 `0x18` 网络重配事件。
- 找到显示 RAM 的两组独立灯组掩码；可配置阈值与固定灯色比较值的关系需要区分。
- 原厂 MCU 更新使用内部 Flash 暂存区，再由 Bootloader 搬运；目前仅分析，不实现刷写。

详细依据：[固件逻辑分析](docs/stock-analysis.md)、[引脚与驱动](docs/hardware.md)、[串口协议](docs/protocol.md)、[开发路线](docs/roadmap.md)。

## 编译与验证

主机测试只需要 C 编译器、Make 和 Python 3：

```sh
make test
make sanitize
```

ARM 编译另需 GNU Arm Embedded 工具链：

```sh
# Debian / Ubuntu
sudo apt install --no-install-recommends gcc-arm-none-eabi binutils-arm-none-eabi
make link-check
```

`make link-check` 校验 Cortex-M0+ 编译、`0x4000` 向量表、32 KiB SRAM 和保留 Flash 区域。没有 UART 或传感器驱动的镜像无法维持设备原有功能；不要把这个 ELF 当成可刷固件。

本次验证结果及边界见 [validation.md](docs/validation.md)。公开 CI 不需要原厂镜像。

## 使用自己的备份复现分析

原厂镜像未随仓库分发。把自己的镜像放在忽略目录 `private/`，或使用仓库外路径：

```sh
python3 tools/analyze_stock.py /path/to/your/m1-mcu-backup.bin
python3 -m venv build/analysis-venv
build/analysis-venv/bin/pip install -r requirements-analysis.txt
build/analysis-venv/bin/python tools/disassemble.py /path/to/your/m1-mcu-backup.bin 0x7e68 0x81d8
build/analysis-venv/bin/python tools/verify_stock.py /path/to/your/m1-mcu-backup.bin
```

分析脚本输出镜像哈希、向量表和任务入口；只有哈希匹配已分析样本时才标注固定地址的任务名。可选差分验证器在 Unicorn 的模拟内存中执行原厂纯函数/指定代码块，对照开源实现，不连接设备。

## 范围与许可

本项目是社区项目。原创实现与文档采用 [MIT](LICENSE)；不包含原厂二进制、反编译源码、厂商 SDK 或设备私有日志。研究范围和来源见 [PROVENANCE.md](PROVENANCE.md)。

联网模块社区固件位于 [phicomm-community-firmware](https://github.com/phicomm-io/phicomm-community-firmware)。本仓库负责 ATSAMD20 主控，两者不是同一个 OTA 镜像。
