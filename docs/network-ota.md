# 经 Wi-Fi 模块升级主控

需要已实现 `POST /api/v1/mcu/exchange` 的配套 M1 联网模块固件。电脑通过该接口发送有边界的二进制 UART 帧；设备上的现有 Web 密码认证同样适用。网络主控升级与 Wi-Fi OTA、配置变更、重启互斥；单次转发等待应答不超过 2.5 秒，升级链路空闲租期 15 秒。

先构建 `make ota`。`build/ota/m1-mcu-ota.bin` 是链接于 `0x4000` 的 SAMD20G18A 应用，不是联网模块镜像，不包含 Bootloader。配套 JSON 的目标、长度、向量、SHA-256 和 UART 版本必须匹配。此工具不接受全片备份，不修改 Bootloader，不执行解锁。

```sh
# 默认只校验本地镜像和联网设备身份/版本
python3 tools/m1_network.py --host DEVICE_IP --device-id DEVICE_ID \
  --image build/ota/m1-mcu-ota.bin --manifest build/ota/m1-mcu-ota.json
# 确定目标后开始传输；密码通过 M1_WEB_PASSWORD 环境变量传入
python3 tools/m1_network.py --host DEVICE_IP --device-id DEVICE_ID \
  --image build/ota/m1-mcu-ota.bin --manifest build/ota/m1-mcu-ota.json --flash
```

工具逐块等待 `0x14` 成功应答、检查索引、有限重试，再提交 `0x16`。最后一个尾块包括长度恰为 256 倍数时的空尾块；Wi-Fi 端必须收到独立 `0x15` 完成通知才允许提交。提交应答只证明 UART 已发送，不证明已启动。工具随后等待同一设备返回镜像清单的主控版本，超时则报告未确认，不自动重新刷入。

主控升级会按已有 PA24 时序复位联网模块，因此网络暂时断开是预期现象。完整备份、首次升级及恢复仍需结合自己的 Bootloader 和硬件状态；Flash 模型测试不替代设备断电恢复验证。
