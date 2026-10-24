# Tag 自动发布

沿用 Wi-Fi 固件仓库的原则：先提交每版说明，再用不可移动的版本 tag 冻结来源；构建和校验通过后才发布。MCU 目前直接维护本仓库 [Release-Log.md](Release-Log.md)，不依赖外部 Wiki。

1. 在 Release Log 增加唯一的 `## vMAJOR.MINOR.PATCH` 章节，写清变化和验证限制；缺失、重复、过短或含 TODO/TBD/待补充等占位内容会阻止发布。
2. 将源码和说明提交到 `main`，本地运行 `make test sanitize ota hal-check`（HAL 环境见仓库 README）。
3. 对该干净提交创建新 tag；版本号从 `v0.9.0` 起递增，不移动旧 tag。例如下一版本是 `v0.9.2` 时：

```sh
git tag -a v0.9.2 -m 'M1 MCU v0.9.2 development preview'
git push --atomic origin main v0.9.2
```

仅 tag 的 **push** 事件自动发布；普通分支、PR、手动运行只验证并保存候选。发布 job 依赖完整验证 job，并再次核对精确 tag、干净源码和 `main` 祖先关系。只有发布 job 获得 `contents: write` 权限；同一 tag 的发布串行执行。

CI 从该次运行下载 OTA 和调试包，核对版本/tag/完整 commit/dirty、原始应用 SHA、Boot SHA、时钟报告与 ELF SHA。`tools/release/prepare.py` 提取冻结说明并生成六个附件：OTA bin/json、调试恢复 ZIP、notes.md、release.json、SHA256SUMS。`release.json` 记录 Release Log SHA 与附件大小/哈希，SHA256SUMS 也覆盖该记录。OTA bin 不增加头部或签名，不改变字节。

发布器先创建草稿，上传并下载回读全部附件，逐字节核对后公开为 **Pre-release**，不设为 Latest。重跑时相同已发布内容只核对；部分完成的草稿只补缺失文件。已有不同说明/附件或缺附件的公开发布均报错，不覆盖。失败后从 Actions 重跑原 tag 的工作流，禁止通过移动 tag 解决问题。

当前策略固定为开发预览，要求输入候选 `hardware_validation=NOT_RUN, release=false`；发布 OTA manifest 改为 `release=true, prerelease=true`，不改变实机验证状态。将来有实机验收后，应先更新发布策略和验证证据，再考虑正式稳定版。此流程不自动刷机、不更新 Wi-Fi MCU OTA 地址，也未接入 Wi-Fi 固件仓库的签名/镜像站部署。
