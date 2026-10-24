# oss-0.3.1 验证记录

日期：2026-10-24。结果对应本次运行时、硬件驱动及 OTA/Boot 实现；后续提交以重新执行的结果为准。

## 启动故障与修复

用户报告 `05a4822`（`oss-0.3`）的 Actions 镜像无法启动，且应用区、暂存区与指定镜像逐字节一致，原厂 Bootloader 未改变。这些是用户提供的实机调查结果，本次代码修复未连接目标板独立复测。

已确认的代码错误：GCLK2 的 DIV 只有 5 位，旧代码以直接分频方式写入 32，硬件会忽略超出字段的位，实际 DIV=0，等于不分频。因此 WDT 的 8192 周期由预期 `8192/(32768/32)=8 s` 变成 `8192/32768=0.25 s`。应用在 PA24 置高后有 1000×1500 次启动忙等，期间未喂狗，因此存在启动被 WDT 打断的路径。是否为实机全部故障的原因，仍需临时寄存器修正或修复镜像的设备测试确认。

依据：[Microchip SAM D20 数据手册](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU32/ProductDocuments/DataSheets/SAM-D20-Family-Data-Sheet-DS60001504.pdf)，§14.8.5（GCLK2 DIV[4:0] 与忽略高位）、§14.8.4（DIVSEL=1 时除数为 `2^(DIV+1)`）、§17.8.2（PER=0xA 对应 8192 周期）。

`oss-0.3.1` 修复为 DIV=4、DIVSEL=1，增加分频/控制寄存器回读和 WDT CONFIG 同步等待。应用与独立 Boot 共用该初始化；保留原厂 Boot 的设备只需验证修复应用，不要求更换 Boot。版本查询 `0x11` 用于区分新候选。

新增 `tools/verify_platform_clock.py` 实际执行构建 ELF 中的 `m1_platform_clock`，模型分别实现 GCLK0/1/2 的 8/16/5 位字段、间接寄存器选择、GCLK/WDT 同步和 enable-protected 配置。应用和 Boot 各覆盖冷启动、已启用 WDT 的再次初始化、ALWAYSON 配置保留、分频写入丢失及 DIVSEL 写入丢失，共 10 例。该模型不模拟整机、看门狗真实倒计时或外设波形；此前的主机假 I/O 和 ELF 链接检查没有覆盖这一缺口。

负面对照使用 Actions 运行 `40141001273` 下载的旧应用/Boot ELF：两者均被新检查拒绝（旧代码还缺少 CONFIG 写入后的同步等待）。失败时模型记录的 GCLK2 DIV 均为 0、频率 32768 Hz，对应 8192 周期仅 0.25 秒。这是旧二进制在寄存器模型中的结果，未当作设备波形实测。

| 层次 | 结果 | 覆盖与边界 |
| --- | --- | --- |
| 普通协议核心 | PASS | 帧流恢复、参数、负温度/−0.01°C、日历、传感器校验和灯组边界 |
| 运行时主机测试 | PASS | 三路收帧、超时/坏帧失效和恢复、PM 双模式/八字节应答/四帧预热/突变/噪声重同步、甲醛处理、RTC、按键、蜂鸣器、I²C 失败恢复策略、计时回绕；SHT 状态 CRC、限流软复位、PWM/屏幕调试租期及 OTA 互斥 |
| 温湿度边界/随机 | PASS | 50000 次有界随机输入，在 ASan/UBSan 下检查输出和溢出 |
| OTA / Boot Flash 模型 | PASS，602 例 | 长度/尾块边界、最大镜像、CRC、乱序/重传、逐点读/擦/写失败、半行擦除/半页写入、重启重做、串口到 Boot 的完整链路 |
| Python 分析/打包工具 | PASS，6 例 | 未知哈希/长度/擦除镜像、OTA 向量和尾块、manifest/SHA、Python 块 CRC 与原厂对照过的 C 接收器一致 |
| 实际构建 OTA 负载 | PASS | 16256 字节原始应用，输入升级模型接收/提交/Boot 安装并逐字节比较；不等于设备 OTA |
| 新增状态机边界 | PASS | 12000 组调试参数/流输入、非法通道、PA24 延时/迟到调度/回绕、未完整镜像不复位 |
| ASan / UBSan | PASS | 运行时、升级及恢复测试；Flash 模型要求擦除后每页只编程一次，拒绝 0→1 |
| 应用 ARM 构建 | PASS | Cortex-M0+；41 项向量、`0x4000`、SP `0x20008000`、实际中断入口和可加载段 |
| Boot ARM 构建 | PASS | 起点 `0`，应用/暂存/元数据分区隔离；独立 Boot 入口 |
| 原厂指令对照 | PASS（沿用 oss-0.3 记录） | 1775 例及 PM 26 例、PA24 四路径、SHT 状态调用，详见下表；本次只修改平台时钟和版本号，未重跑原厂代码对照 |
| ARM 时钟 MMIO 模型 | PASS，10 例 | 实际应用/Boot ELF；字段宽度、1024 Hz/8 s、同步、冷/再次初始化、ALWAYSON 和配置丢失 |
| 真实寄存器/外设波形 | NOT_RUN | HAL 已编译，但主机假 I/O/Unicorn 不能验证真实硬件 |
| oss-0.3 实机启动 | FAIL（用户报告） | 指定 Actions 镜像无法启动；应用/暂存一致、原厂 Boot 未改；本次未独立复测 |
| oss-0.3.1 实机启动 / OTA / 断电恢复 | NOT_RUN | 本次未连接或写入设备；等待修复候选实机复验 |

## 原厂指令对照

脚本只接受精确匹配的备份 SHA-256。原有两脚本只映射模拟 Flash/SRAM；新增 PA24 工具额外映射模拟 GPIO/AIRCR 页并记录原厂写指令，不转发到物理设备。传感器 I/O、调度/信号量和部分除法辅助函数用显式替身；原厂整数乘法、四状态模型、数字段表和显示渲染在新对照中实际执行。

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

新增 `verify_peripherals_stock.py` 实际执行 `0x78cc`，在模式 2/3 中各输入 13 个候选（预热、边界突升、错误字节、恢复），逐步与新 C 运行时输出对比；模式 3 对照每个 64 字节窗口仅一个对齐候选，不据此声称原厂全部窗口布局等价。四条 PA24 路径确认启动置高、取消不拉低、完整/不完整非取消分支均 4000 tick→低→1000 tick→系统复位；GPIO 配置、RTOS 和 NVM 仍为替身。SHT 原厂 `0x5868` 确认 f32d→读 3 字节→CRC 检查；软复位恢复策略由官方资料和主机状态机测试支持，未当作原厂全路径复现。

机器可读结果：[stock-verification.json](stock-verification.json)、[runtime-stock-verification.json](runtime-stock-verification.json)、[peripheral-stock-verification.json](peripheral-stock-verification.json)。随机种子 `20261024`。这是选定计算/代码块的对照，不是整个原厂固件、原厂 OTA 或 MMIO 仿真。

## 构建产物

GNU Arm Embedded GCC 13.2.rel1，本地实际构建：

| 镜像 | text | data | bss | 地址 |
| --- | --- | --- | --- | --- |
| `build/m1-app.bin` / `.elf` | 16252 | 4 | 9680 | `0x4000` |
| `build/m1-boot.bin` / `.elf` | 2012 | 0 | 4 | `0` |

单位为字节。Boot 不链接应用 UART 中断队列；保留 32 KiB RAM 中的栈空间。ELF 检查器读取真实段和向量，不是只匹配链接脚本文本。逐镜像 `.json` 记录实际二进制 SHA-256、工具链、源码 commit/dirty 状态及 `hardware_validation=NOT_RUN`，不会据此自动刷写。

## 复现

```sh
make test sanitize
make ota
python3 -m venv build/analysis-venv
build/analysis-venv/bin/pip install -r requirements-analysis.txt
make hal-check HAL_PYTHON=build/analysis-venv/bin/python
build/analysis-venv/bin/python tools/verify_stock.py /path/to/private-backup.bin
build/analysis-venv/bin/python tools/verify_runtime_stock.py /path/to/private-backup.bin
build/analysis-venv/bin/python tools/verify_peripherals_stock.py /path/to/private-backup.bin
```

公开 CI 运行主机测试、ASan/UBSan、两类 ARM 构建、ARM 时钟 MMIO 模型及实际 OTA 负载的接收/搬运模型，上传 OTA 候选和调试/恢复产物（含 `platform-clock-verification.json`）；原厂镜像需要用户自己的备份，不进入公开 CI。自 `v0.9.1` 起版本 tag 自动发布 GitHub 开发预览 Release，仍标记 `hardware_validation=NOT_RUN`，不操作设备。下一步仍需独立样机上的启动/驱动/联网互通及受控升级恢复验收。
