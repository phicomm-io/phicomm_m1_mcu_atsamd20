# 验证记录

日期：2026-10-24。适用初始工程；后续提交以 CI 和重新执行的结果为准。

| 层次 | 结果 | 覆盖与边界 |
| --- | --- | --- |
| 原厂镜像识别 | PASS | 主 Flash 262144 字节、向量、哈希、任务入口及选定字符串，见 [stock-image.json](stock-image.json) |
| C 主机测试 | PASS | 普通帧、噪声/截断恢复、负温度与 −0.01°C、日期闰日、参数拒绝、未实现 OTA 不改变状态、传感器坏校验与灯组边界 |
| Python 分析工具测试 | PASS | 错误镜像长度拒绝、未知哈希不套用固定地址标签、擦除镜像 |
| ASan / UBSan | PASS | C 主机测试在地址/未定义行为检查下通过 |
| ARM 编译/链接 | PASS | Cortex-M0+，41 项向量、起点 `0x4000`，初始 SP `0x20008000`，Flash/SRAM 边界 |
| 原指令差分 | PASS，631 例 | 详见下表，不等于完整固件仿真 |
| UART / 传感器 / 屏幕 / 蜂鸣器实物 | NOT_RUN | 尚无可运行完整驱动 |
| 主控 Flash 写入、OTA、掉电恢复 | NOT_RUN | 本次没有刷写设备，也没有发布升级包 |

## 差分验证

`tools/verify_stock.py` 要求镜像 SHA 精确匹配，使用 Unicorn 执行备份中的选定 Thumb 函数或代码块，同时调用本项目 C 实现对照。只映射 Flash/SRAM；不映射 MMIO，不连接 J-Link 或网络设备。数字格式化/信号量/除法辅助路径有显式替身；因此渲染比较只覆盖灯组位，不覆盖数字、RTOS 或硬件时序。

| 对照项 | 原厂入口 | 例数 |
| --- | --- | --- |
| OTA 单字节 CRC 更新 | `0x84e0` | 512（初值 0 全部 256 字节 + 固定种子随机初值） |
| 温湿度双字节 CRC8 | `0x5660` | 64 |
| 两类灯组写入 | `0x69c4/0x6c90` | 16（有效组号交叉，随机 RAM 初值） |
| HCHO/PM 选择边界 | `0x6ad8/0x6dac` | 38（每类 19 个边界/范围值） |
| 带负温度和三个状态的采样编码 | `0x85a0–0x85ec` | 1 |

机器可读结果：[stock-verification.json](stock-verification.json)。随机种子为 `20261024`，结果可复现。样本编码只有一例原指令对照，其余格式边界由独立主机测试覆盖，不能称为采样编码全范围原指令验证。

## 复现命令

```sh
make test sanitize
make link-check
python3 tools/analyze_stock.py /path/to/private-backup.bin
python3 -m venv build/analysis-venv
build/analysis-venv/bin/pip install -r requirements-analysis.txt
build/analysis-venv/bin/python tools/verify_stock.py /path/to/private-backup.bin
```

本地 ARM 工具链为 GNU Arm Embedded GCC 13.2.rel1。初始链接夹具 text 2224 字节、data 0、bss 56；这是带协议核心的离线夹具大小，不是完整固件资源消耗。向量检查器会核验真实 ELF 的可加载段，而非只读取链接脚本文本。

公开 GitHub Actions 运行主机测试、ASan/UBSan 和 ARM 链接检查。原厂差分校验需要用户自己的备份，不放入公共 CI；CI 不上传固件、不发布 release，也不执行硬件写入。
