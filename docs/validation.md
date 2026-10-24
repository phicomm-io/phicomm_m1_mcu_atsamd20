# oss-0.2 验证记录

日期：2026-10-24。结果对应本次运行时、硬件驱动及 OTA/Boot 实现；后续提交以重新执行的结果为准。

| 层次 | 结果 | 覆盖与边界 |
| --- | --- | --- |
| 普通协议核心 | PASS | 帧流恢复、参数、负温度/−0.01°C、日历、传感器校验和灯组边界 |
| 运行时主机测试 | PASS | 三路收帧、超时/坏帧失效和恢复、PM 预热/突变、甲醛处理、RTC、按键、蜂鸣器、I²C 失败恢复策略、计时回绕 |
| 温湿度边界/随机 | PASS | 50000 次有界随机输入，在 ASan/UBSan 下检查输出和溢出 |
| OTA / Boot Flash 模型 | PASS，602 例 | 长度/尾块边界、最大镜像、CRC、乱序/重传、逐点读/擦/写失败、半行擦除/半页写入、重启重做、串口到 Boot 的完整链路 |
| Python 分析工具 | PASS，3 例 | 未知哈希不套固定地址、镜像长度检查、擦除镜像 |
| ASan / UBSan | PASS | 运行时、升级及恢复测试；Flash 模型要求擦除后每页只编程一次，拒绝 0→1 |
| 应用 ARM 构建 | PASS | Cortex-M0+；41 项向量、`0x4000`、SP `0x20008000`、实际中断入口和可加载段 |
| Boot ARM 构建 | PASS | 起点 `0`，应用/暂存/元数据分区隔离；独立 Boot 入口 |
| 原厂指令对照 | PASS，1775 例 | 原有 631 例 + 补偿 944 例 + 全屏渲染 200 例，详见下表 |
| 真实寄存器/外设波形 | NOT_RUN | HAL 已编译，但主机假 I/O/Unicorn 不能验证真实硬件 |
| 实机 Flash / OTA / 断电恢复 | NOT_RUN | 本次未连接或写入设备；没有发布刷机版本 |

## 原厂指令对照

脚本只接受精确匹配的备份 SHA-256。仅映射 Flash/SRAM，不映射 MMIO，不连接设备。传感器 I/O、调度/信号量和部分除法辅助函数用显式替身；原厂整数乘法、四状态模型、数字段表和显示渲染在新对照中实际执行。

| 项目 | 原厂入口 | 例数 |
| --- | --- | --- |
| bit-input CRC16 | `0x84e0` | 512 |
| SHT 双字节 CRC8 | `0x5660` | 64 |
| 两类灯组随机 RAM 写入 | `0x69c4/0x6c90` | 16 |
| HCHO/PM 选择边界 | `0x6ad8/0x6dac` | 38 |
| 带负温度/三个状态的采样编码 | `0x85a0–0x85ec` | 1 |
| 温湿度连续状态及输出 | `0x58c4` | 800，八个连续 100 步序列，含显示切换 |
| 温湿度冷启动极限输入 | `0x58c4` | 144，原始字端点/湿度/额外输入/显示组合 |
| 完整 29 字节显示 | `0x66dc` 至 `0x705c` | 200，数字、负号、单位、日期/时间、超限和灯组 |

机器可读结果：[stock-verification.json](stock-verification.json)、[runtime-stock-verification.json](runtime-stock-verification.json)。随机种子 `20261024`。这是选定计算/代码块的对照，不是整个原厂固件、原厂 OTA 或 MMIO 仿真。

## 构建产物

GNU Arm Embedded GCC 13.2.rel1，本地实际构建：

| 镜像 | text | data | bss | 地址 |
| --- | --- | --- | --- | --- |
| `build/m1-app.bin` / `.elf` | 13544 | 0 | 9576 | `0x4000` |
| `build/m1-boot.bin` / `.elf` | 1980 | 0 | 4 | `0` |

单位为字节。Boot 不链接应用 UART 中断队列；保留 32 KiB RAM 中的栈空间。ELF 检查器读取真实段和向量，不是只匹配链接脚本文本。逐镜像 `.json` 记录实际二进制 SHA-256、工具链、源码 commit/dirty 状态及 `hardware_validation=NOT_RUN`，不会据此自动刷写。

## 复现

```sh
make test sanitize
make firmware bootloader
python3 -m venv build/analysis-venv
build/analysis-venv/bin/pip install -r requirements-analysis.txt
build/analysis-venv/bin/python tools/verify_stock.py /path/to/private-backup.bin
build/analysis-venv/bin/python tools/verify_runtime_stock.py /path/to/private-backup.bin
```

公开 CI 运行主机测试、ASan/UBSan 和两类 ARM 构建；原厂镜像需要用户自己的备份，不进入公开 CI。CI 不上传固件、不发布 release、不操作设备。下一步仍需独立样机上的启动/驱动/联网互通及受控升级恢复验收。
