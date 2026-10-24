# 构建和下载 MCU OTA 镜像

版本发布从 [GitHub Releases](https://github.com/phicomm-io/phicomm_m1_mcu_atsamd20/releases) 下载。自 `v0.9.1` 起推送版本 tag，CI 通过后自动发布开发预览：包含下列 OTA 文件、`m1-mcu-debug-recovery.zip`、冻结的 `notes.md` 和 `release.json`。Release 的 `SHA256SUMS` 覆盖其余五个附件；下载全部附件后运行 `sha256sum -c SHA256SUMS`。版本由 `v0.9.0` 起步的 Git tag 加六位 commit hash 生成，见 [版本规则](versioning.md)。

分支/tag push、pull_request 和手动 workflow_dispatch 也生成 Actions 候选，保存 30 天。入口为 **Actions → Firmware, OTA artifacts and recovery checks → 成功的运行 → Artifacts**，下载 `m1-mcu-ota-<version>`，例如 `m1-mcu-ota-0.9.0-abcdef`。PR 构建对应合并测试提交，候选来源以包中 manifest 的 source_commit 为准。

该包包含：

| 文件 | 用途 |
| --- | --- |
| `m1-mcu-ota.bin` | 链接于 `0x4000` 的原始 ATSAMD20G18A 应用，无头部、无 Boot、无签名、无额外填充 |
| `m1-mcu-ota.json` | 版本/tag/完整 commit hash/dirty、UART 版本、长度、SHA-256、编译器、完整块数/尾长、地址与验证状态 |
| `SHA256SUMS` | Actions 包校验上述两文件；Release 包校验全部其他附件 |

Actions 的 `m1-mcu-debug-recovery-<version>` 单独提供应用 ELF/map、Boot bin/ELF/map、版本快照、时钟模型报告及构建记录。Release 的调试恢复 ZIP 另外包含同一份应用 bin 和地址说明。**不要将 Boot 或整个 256 KiB 备份作为应用 OTA 负载。** 这是主控 MCU 升级，不能直接填入 EMW3080 联网模块固件更新地址。

本地构建：

```sh
make test sanitize
make ota
# 非默认工具链：
make ota CROSS_COMPILE=/path/to/arm-none-eabi-
```

产物位于 `build/ota/`。构建流程检查应用 ELF/向量/分区，打包器核对 manifest 与 payload、镜像 164–65535 字节、栈和 Thumb 入口；然后把该实际二进制输入 C 升级/Flash 模型，验证接收、提交和 Boot 搬运后的应用逐字节相同。CI 无需原厂备份；版本 tag 自动发布开发预览 Release，不自动刷机或更改线上 OTA 地址。

## Wi-Fi MCU 发送流程

1. 校验下载长度与 SHA-256。完整块数 `N=length/256`，尾长 `T=length%256`，均为整数运算，N/T 不超过 255。
2. 普通 `0x12` 帧传 N/T，等待 `0x14`。随后从序号 1 开始发送 N 个 256 字节块；每块使用 `0x13` 可变长帧并等待该序号 `0x14` 应答成功。失败可重发当前块，不能跳块。
3. 始终发送序号 N+1 的尾块；T=0 时也必须发送没有负载的 6 字节尾帧。
4. 最后尾块成功后等待 `0x15 f2=1`，再发 `0x16 f2=0` 提交。`0x16 f2=1` 表示取消，不能当作提交。
5. 新固件提交后等待 4000 ms 拉低 PA24/CHIP_EN，再保持低电平至少 1000 ms 请求主控复位。Boot 完成后应用释放 CHIP_EN，Wi-Fi MCU 会重新启动。重新建立 UART 后查询 `0x11`，结合 `0x17` 启动通知确认新版本；不能仅凭接收完成判断升级成功。

CRC、序号及应答字段见 [protocol.md](protocol.md)。`tools/pack_ota.py` 的 `frames(data)` 提供离线帧生成函数，可由 Wi-Fi MCU/主机发送器参考；它不负责应答、重试、超时，也不会把所有帧不加等待地写入串口。其块 CRC 与 C 接收器有跨语言对照测试。

## 兼容与验证范围

协议及 `0x4000/0x22000/0x3ff00` 布局沿用本次原厂备份。保留原厂 Boot 时只能获得原厂搬运行为；新 Boot 才校验额外 `M1UP` CRC32 日志并从完整暂存重做中断搬运。原厂 Boot 与新 Boot 的真实升级均未在本次运行。已有 Wi-Fi 固件还需真正接入主控下载及串口发送流程；Actions 产物下载本身不会触发设备升级。

所有产物标记 `hardware_validation=NOT_RUN`。本地/Actions 候选标记 `release=false`，发布的 OTA manifest 标记 `release=true, prerelease=true`；二进制字节保持相同，发布标记不表示通过实机验收。恢复 ZIP 保留原始构建记录的候选标记。SHA/CRC 是完整性检查，不是镜像签名。当前是可供升级链路集成验证的开发候选镜像；Boot 恢复边界见 [implementation.md](implementation.md)。
