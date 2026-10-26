# Tag 自动发布

沿用 Wi-Fi 固件仓库的原则：先提交每版说明，再用不可移动的版本 tag 冻结来源；构建和校验通过后才发布。MCU 目前直接维护本仓库 [Release-Log.md](Release-Log.md)，不依赖外部 Wiki。

1. 在 Release Log 增加与 tag 完全一致的唯一章节，如 `## v1.0.0-rc`，写清变化和验证限制；缺失、重复、过短或含 TODO/TBD/待补充等占位内容会阻止发布。
2. 将源码和说明提交到 `main`，本地运行 `make test sanitize ota hal-check`（HAL 环境见仓库 README）。
3. 保持 `VERSION=1.0.0`，对该干净提交创建新 RC tag，不移动旧 tag。例如：

```sh
git tag -a v1.0.0-rc -m 'M1 MCU v1.0.0 release candidate'
git push --atomic origin main v1.0.0-rc
```

仅 tag 的 **push** 事件自动发布；普通分支、PR、手动运行只验证并保存候选。发布 job 依赖完整验证 job，并再次核对精确 tag、干净源码和 `main` 祖先关系。只有发布 job 获得 `contents: write` 权限；同一 tag 的发布串行执行。

CI 从该次运行下载 OTA 和调试包，核对版本/tag/完整 commit/dirty、原始应用 SHA、Boot SHA、时钟报告与 ELF SHA。`tools/release/prepare.py` 提取冻结说明并生成六个附件：OTA bin/json、调试恢复 ZIP、notes.md、release.json、SHA256SUMS。`release.json` 记录 Release Log SHA 与附件大小/哈希，SHA256SUMS 也覆盖该记录。OTA bin 不增加头部或签名，不改变字节。

RC 和 0.x 预览先创建草稿，上传并下载回读全部附件，逐字节核对后以 `--prerelease --latest=false` 公开，不取代正式稳定版。重跑时相同已发布内容只核对；部分完成的草稿只补缺失文件。已有不同说明/附件或缺附件的公开发布均报错，不覆盖。失败后从 Actions 重跑原 tag 的工作流，禁止通过移动 tag 解决问题。

输入候选必须为 `hardware_validation=NOT_RUN, release=false`。RC 发布的 OTA manifest 改为 `release=true, prerelease=true`，保留 NOT_RUN；不读取正式版验收文件。原厂 11.x 用户可优先使用，17.1.2.3 / 17.x 仍待镜像分析后确认兼容性。UART 保留完整 RC 版本和六位源码 hash，超过 16 字符会阻止发布；当前应使用无序号 `v1.0.0-rc`，详见[版本规则](../versioning.md)。

1.x 及以上不带 RC 的正式 tag 仍要求该 tag、完整源码 SHA、App/Boot SHA 全部匹配的实机验收记录，RC 不会解除这一门槛。此流程不自动刷机、不更新 Wi-Fi MCU OTA 地址，也未接入 Wi-Fi 固件仓库的签名/镜像站部署。
