# 验证方法

本地运行：

```sh
make test sanitize
make ota
make hal-check HAL_PYTHON=build/hal-venv/bin/python
```

主机检查覆盖协议、传感器解析、显示、RTC、按键、告警、音乐和升级中断；ARM 检查覆盖向量、分区、时钟初始化以及实际 OTA 负载的接收与搬运。原厂指令对照需要用户自己的备份，不随仓库分发。

设备验收应检查启动、采样与断线恢复、屏幕和物理控制、RTC 保时、联网互通及受控升级恢复。软件测试不能代替真实外设和供电中断验证。

原厂 11.x 为当前推荐的兼容来源。17.1.2.3 / 17.x 差异较大，待取得镜像分析后再确认。保留原厂 Boot 与替换独立 Boot 的升级恢复范围不同，见 [实现说明](implementation.md)。
